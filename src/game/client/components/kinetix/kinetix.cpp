#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

// =========================================================
// CONSTRUCTOR / DESTRUCTOR
// =========================================================

CBotNet::CBotNet()
{
        // v1.56.171 BUG9: all kx_ on/off + numeric settings are now MACRO_CONFIG_INT
        // cvars (defaults in config_variables.h). Constructor no longer initializes them.
        for(int i = 0; i < 128; i++)
        {
                m_TargetList[i] = false;
                m_BotsList[i] = false;
                m_RescueList[i] = false;
        }

        // String buffers for UI input fields
        m_aTargetIDsStr[0] = '\0';
        m_aBotsIDsStr[0] = '\0';
        m_aRescueIDsStr[0] = '\0';

        // Pathfinder tab (Kinetix→Pathfinder) — chunk-based mini TAS maker.
        m_PfState = PF_STATE_IDLE;
        m_PfChunkCount = 0;
        m_PfStartPos = vec2(0, 0);
        m_PfCurPos = vec2(0, 0);
        m_PfCurVel = vec2(0, 0);
        m_PfCurHookState = 0;       // HOOK_IDLE
        m_PfCurHookPos = vec2(0, 0);
        m_PfCurHookDir = vec2(0, 0);
        m_PfCurHookTick = 0;
        m_PfCurFreezeTime = 0;
        m_PfCurJumped = 0;
        m_PfTickCounter = 0;
        m_PfFlowField = nullptr;
        m_PfTotalFreezeTicks = 0;
        m_PfBacktrackIdx = 0;

        // State-Lattice A* state
        m_PfANodes.clear();
        m_PfAOpen.clear();
        m_PfABestG.clear();
        m_PfAGoalIdx = -1;
        m_PfAStarted = false;
        m_PfAPathReady = false;
        m_PfAExpandCount = 0;
        m_PfFullInputs.clear();
        m_PfFullInputsIdx = 0;

        m_MapWidth = 0;
        m_MapHeight = 0;
        m_pMapGrid = nullptr;
        m_pFrontGrid = nullptr;
        m_PfPlayerPenalty = nullptr;
        m_MapGridLoaded = false;
        m_pfDist = nullptr;
        m_pfVisited = nullptr;
        m_aLastMapName[0] = '\0';
        m_PfEditorMode = 0;
        m_pForbiddenGrid = nullptr;
        m_pCustomFinishGrid = nullptr;

        // Per-dummy state is auto-initialized by CBotNetDummy constructor
}

CBotNet::~CBotNet()
{
        PfThreadStop();
        if(m_PfBaseWorld) { delete m_PfBaseWorld; m_PfBaseWorld = nullptr; }
        if(m_pMapGrid) delete[] m_pMapGrid;
        if(m_pFrontGrid) delete[] m_pFrontGrid;
        if(m_pfDist) delete[] m_pfDist;
        if(m_pfVisited) delete[] m_pfVisited;
        if(m_PfPlayerPenalty) delete[] m_PfPlayerPenalty;
        if(m_PfFlowField) delete[] m_PfFlowField;
        if(m_PfScoreField) delete[] m_PfScoreField;
        if(m_pForbiddenGrid) delete[] m_pForbiddenGrid;
        if(m_pCustomFinishGrid) delete[] m_pCustomFinishGrid;
}

bool CBotNet::OnInput(const IInput::CEvent &Event)
{
        if(m_PfEditorMode == 0)
                return false;
        if(g_Config.m_ClClickGui != 0)
                return false;
        CGameClient *pGame = GameClient();
        if(!pGame || pGame->m_Menus.IsActive())
                return false;
        if(Event.m_Key != KEY_MOUSE_1 && Event.m_Key != KEY_MOUSE_2)
                return false;
        return (Event.m_Flags & (IInput::FLAG_PRESS | IInput::FLAG_RELEASE)) != 0;
}

static void ConDummySwitch(IConsole::IResult *pResult, void *pUserData)
{
        ((CBotNet *)pUserData)->SmartDummySwitch();
}

void CBotNet::SmartDummySwitch()
{
        const int Cur = g_Config.m_ClDummy;
        int Next = -1;
        if(g_Config.m_KxSdsMode == 0)
        {
                for(int Off = 1; Off <= MAX_DUMMIES && Next < 0; Off++)
                {
                        const int D = (Cur + Off) % MAX_DUMMIES;
                        if(D == 0 || Client()->DummyConnected())
                                Next = D;
                }
        }
        else
        {
                int aIds[MAX_DUMMIES];
                int NumIds = 0;
                const char *pCursor = g_Config.m_KxSdsDummyIds;
                while(*pCursor && NumIds < MAX_DUMMIES)
                {
                        while(*pCursor == ' ' || *pCursor == '\t')
                                pCursor++;
                        if(*pCursor < '0' || *pCursor > '9')
                                break;
                        int V = 0;
                        while(*pCursor >= '0' && *pCursor <= '9')
                        {
                                V = V * 10 + (*pCursor - '0');
                                pCursor++;
                        }
                        if(V < MAX_DUMMIES)
                                aIds[NumIds++] = V;
                        while(*pCursor && *pCursor != ',')
                                pCursor++;
                        if(*pCursor == ',')
                                pCursor++;
                }
                if(NumIds > 0)
                {
                        int Pos = -1;
                        for(int i = 0; i < NumIds; i++)
                        {
                                if(aIds[i] == Cur)
                                {
                                        Pos = i;
                                        break;
                                }
                        }
                        const int Start = Pos >= 0 ? Pos : NumIds - 1;
                        for(int Off = 1; Off <= NumIds && Next < 0; Off++)
                        {
                                const int D = aIds[(Start + Off) % NumIds];
                                if(D == 0 || Client()->DummyConnected())
                                        Next = D;
                        }
                }
        }
        if(Next >= 0 && Next != Cur)
                g_Config.m_ClDummy = Next;
}

// =========================================================
// ON CONSOLE INIT
// =========================================================

void CBotNet::OnConsoleInit()
{
        // v1.56.171 BUG9: most kx_ commands converted to MACRO_CONFIG_INT cvars.
        // Only complex/temporal commands remain registered here. The cvar-converted
        // commands (kx_attack, kx_aimbot, kx_autoaim, etc.) are now accessible via
        // `toggle kx_attack 1 0` in binds and via direct `kx_attack 1` in console.
        Console()->Register("kx_targets", "s[ids]", CFGFLAG_CLIENT, ConSetTargets, this, "Targets");
        Console()->Register("kx_bots", "s[ids]", CFGFLAG_CLIENT, ConSetBots, this, "Bots (allies)");
        Console()->Register("kx_atk_set", "iiiiiiiiiiiiiiii", CFGFLAG_CLIENT, ConAttackSettings, this, "Settings (16 params)");
        Console()->Register("kx_atk_dists", "fffffff", CFGFLAG_CLIENT, ConAttackDists, this, "Radii");
        Console()->Register("kx_rescue_ids", "s[ids]", CFGFLAG_CLIENT, ConRescueIds, this, "Rescue/Unrescue IDs");
        Console()->Register("kx_pathfinder_go", "i[on] ?i[x] ?i[y]", CFGFLAG_CLIENT, ConPathfinderGo, this, "Move to position");
        Console()->Register("kx_macro_load", "s[path]", CFGFLAG_CLIENT, ConMacroLoad, this, "Load macro from file");
        Console()->Register("kx_macro_play", "i[on]", CFGFLAG_CLIENT, ConMacroPlay, this, "Play loaded macro");
        Console()->Register("kx_macro_record", "i[on]", CFGFLAG_CLIENT, ConMacroRecord, this, "Record macro");
        Console()->Register("kx_macro_save", "s[path]", CFGFLAG_CLIENT, ConMacroSave, this, "Save recorded macro");
        Console()->Register("kx_macro_capture", "i[id]", CFGFLAG_CLIENT, ConMacroCapture, this, "Set capture ID");
        Console()->Register("kx_send", "s[dummyids] r[command]", CFGFLAG_CLIENT, ConSendDummy, this, "Execute command on dummies");

        Console()->Register("kx_pf_live", "i[state]", CFGFLAG_CLIENT, ConPfLive, this, "Pathfinder tab state");
        Console()->Register("kx_pf_paste", "", CFGFLAG_CLIENT, ConPfPaste, this, "Pathfinder: paste path into TAS");
        Console()->Register("kx_dummy_switch", "", CFGFLAG_CLIENT, ConDummySwitch, this, "Smart dummy switch");
}

// =========================================================
// ENGINE LIFECYCLE (CComponent virtuals)
// =========================================================

void CBotNet::OnReset()
{
        g_Config.m_KxAttack = false;
        g_Config.m_KxCopyMoves = false;
        g_Config.m_KxRandomAim = false;
        g_Config.m_KxKinodynamic = false;
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                m_aDummies[D].Reset();
                ResetDummyInputs(D);
        }
}

void CBotNet::OnMapLoad()
{
        m_MapGridLoaded = false;
        m_PfState = PF_STATE_IDLE;
        PfClearSearchState();
        m_PfVPath.clear();

        m_TripleFlyHooking = false;
        m_TripleFlyWasActive = false;
        m_TripleFlyDummy = -1;

        CGameClient *pGame = GameClient();
        if(pGame)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(D == 0)
                                pGame->m_DummyFire = 0;
                        // m_aDummyLastFireTick — нет в BestClient, пропускаем
                }
        }
}

void CBotNet::OnUpdate()
{
        if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        #define KX_CVAR_FALL(prev, cvar) do { \
                int cur = g_Config.cvar; \
                if(prev != cur) { \
                        if(prev && !cur) { for(int D = 0; D < MAX_DUMMIES; D++) ResetDummyInputs(D); } \
                        prev = cur; \
                } } while(0)
        KX_CVAR_FALL(m_PrevKxAttack, m_KxAttack);
        KX_CVAR_FALL(m_PrevKxStand, m_KxStand);
        KX_CVAR_FALL(m_PrevKxAutoAim, m_KxAutoAim);
        KX_CVAR_FALL(m_PrevKxAutoFire, m_KxAutoFire);
        KX_CVAR_FALL(m_PrevKxAutoHook, m_KxAutoHook);
        KX_CVAR_FALL(m_PrevKxMove, m_KxMove);
        KX_CVAR_FALL(m_PrevKxRescue, m_KxRescue);
        KX_CVAR_FALL(m_PrevKxKillFrz, m_KxKillFrz);
        KX_CVAR_FALL(m_PrevKxAtkMain, m_KxAtkMain);
        KX_CVAR_FALL(m_PrevKxHammer, m_KxHammer);
        KX_CVAR_FALL(m_PrevKxSmartDetect, m_KxSmartDetect);
        KX_CVAR_FALL(m_PrevKxSmartRescue, m_KxSmartRescue);
        KX_CVAR_FALL(m_PrevKxAvoidFreeze, m_KxAvoidFreeze);
        KX_CVAR_FALL(m_PrevKxPfHook, m_KxPfHook);
        KX_CVAR_FALL(m_PrevKxCopyMoves, m_KxCopyMoves);
        if(m_PrevKxKinodynamic != g_Config.m_KxKinodynamic)
        {
                if(m_PrevKxKinodynamic && !g_Config.m_KxKinodynamic)
                {
                        for(int D = 0; D < MAX_DUMMIES; D++)
                        {
                                m_aDummies[D].m_KinoCache.Reset();
                                ResetDummyInputs(D);
                        }
                }
                m_PrevKxKinodynamic = g_Config.m_KxKinodynamic;
        }
        if(m_PrevKxAtkPathfinder != g_Config.m_KxAtkPathfinder)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        m_aDummies[D].m_LastTargetTX = -1;
                        m_aDummies[D].m_LastTargetTY = -1;
                        m_aDummies[D].m_PathFound = false;
                }
                m_PrevKxAtkPathfinder = g_Config.m_KxAtkPathfinder;
        }
        if(m_PrevKxPfSimulatePlayers != g_Config.m_KxPfSimulatePlayers)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        m_aDummies[D].m_LastTargetTX = -1;
                        m_aDummies[D].m_LastTargetTY = -1;
                        m_aDummies[D].m_PathFound = false;
                }
                m_PrevKxPfSimulatePlayers = g_Config.m_KxPfSimulatePlayers;
        }
        if(m_PrevKxAtkHookDelay != g_Config.m_KxAtkHookDelay)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        m_aDummies[D].m_HookTickTimer = 0;
                m_PrevKxAtkHookDelay = g_Config.m_KxAtkHookDelay;
        }
        if(m_PrevKxPfHook != g_Config.m_KxPfHook)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        m_aDummies[D].m_PfHookTile = vec2(0, 0);
                m_PrevKxPfHook = g_Config.m_KxPfHook;
        }
        #undef KX_CVAR_FALL

        UpdatePathfinder();
        UpdateLaserUnfreeze();
        UpdateFlyRide();
        UpdateTripleFly();
        UpdatePfTileEditor();

        CGameClient *pGame = GameClient();
        if(!pGame->m_Snap.m_pLocalInfo)
                return;

        if(g_Config.m_KxAttack)
        {
                for(int d = 0; d < MAX_DUMMIES; d++)
                {
                        int id = pGame->m_aLocalIds[d];
                        if(id >= 0 && id < 128 && pGame->m_aClients[id].m_Active)
                                m_BotsList[id] = true;
                }
        }

        if(g_Config.m_KxAutoMain && g_Config.m_KxAttack)
        {
                int ActiveID = pGame->m_aLocalIds[g_Config.m_ClDummy];
                if(ActiveID >= 0 && g_Config.m_KxMain != ActiveID)
                        g_Config.m_KxMain = ActiveID;
        }

        bool anyBotnetFeature = g_Config.m_KxAttack || g_Config.m_KxCopyMoves || g_Config.m_KxRandomAim;
        if(!anyBotnetFeature)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(D == g_Config.m_ClDummy)
                                continue;
                        if(D != 0 && !Client()->DummyConnected())
                                continue;
                        if(m_aDummies[D].m_MacroPlaying || m_aDummies[D].m_PathfinderGoActive)
                        {
                                anyBotnetFeature = true;
                                break;
                        }
                }
        }
        if(!anyBotnetFeature)
                return;

        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                if(D == g_Config.m_ClDummy)
                        continue;
                if(D != 0 && !Client()->DummyConnected())
                        continue;
                int LocalID = pGame->m_aLocalIds[D];
                if(LocalID < 0 || LocalID >= 64)
                        continue;
                if(!pGame->m_aClients[LocalID].m_Active)
                        continue;
                ProcessDummy(D);
        }
}

void CBotNet::OnRender()
{
        RenderPfTileEditor();

        if(m_PfState != PF_STATE_IDLE)
                RenderPathfinderPath();

        RenderLaserUnfreezePath();
        RenderFlyRideAnchor();

        if(!g_Config.m_KxKinodynamic)
                return;

        if(g_Config.m_KxKinoShowField)
                RenderVectorField();
        if(g_Config.m_KxKinoShowPath)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(!IsDummyActive(D))
                                continue;
                        RenderPackage(D);
                }
        }
}
