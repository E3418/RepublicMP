# Ghidra labels for SOVIET64.exe (1.1.1.9)

Addresses are RVAs (image base 0x140000000). Names applied by GhidraMCP; the
comment on each one is `RepublicMP: <note> [<file>]`.

## Functions (rename + decompiler comment)

| RVA | Name | Source | Note |
|---|---|---|---|
| `0x105A90` | `FrameLoop` | determinism/determinism.cpp | main thread frame loop; pacing requests |
| `0x105700` | `Menu_ReloadAssets` | game/world.h | menu <-> game transition reloads texture/mesh state |
| `0x30C790` | `SimStep` | determinism/determinism.cpp | one simulation step (world); hooked inline |
| `0x30D1A0` | `SimLate` | determinism/sim_patches.cpp | Late body: tool handlers 312823..31496d go through RepublicMP tool stubs; autosave, population |
| `0x139A70` | `Early_BuildingPass` | determinism/determinism.cpp | Early's building pass (KEY_SIM_FUNC1); moves doors of buildings on camera |
| `0x699420` | `Sim_VehicleMatrices` | determinism/determinism.cpp | KEY_SIM_FUNC2; matrices rendering touches too |
| `0x317DB0` | `RenderThread_Frame` | determinism/determinism.cpp | render-thread frame; pacing requests, construction-stage drawer |
| `0x42CC70` | `SaveGame` | save/mod_save.cpp | save routine (world); republicmp.ini written after it; lockstep/shared_save |
| `0x430FC0` | `LoadWorld` | determinism/determinism.cpp | loads the save named in the load globals |
| `0x6C6770` | `Save_WriteVehicles` | lockstep/shared_save.h | save writes every vehicle (42cc70 -> 6c6770 -> 6c1bd0) |
| `0x6C1BD0` | `Save_AgeVehicle` | lockstep/shared_save.h | ages a vehicle while saving (by SoylentGreed) |
| `0x334780` | `Economy_PeriodicUpdate` | determinism/determinism.cpp | periodic economy update (prices, era) |
| `0x2FB1D0` | `Economy_PriceEraStep` | determinism/sim_patches.cpp | price and era step; new game's starting money |
| `0x2FCAB0` | `Economy_LoadHistory` | determinism/sim_patches.cpp | called by load when economy history (world+11728) >= 0x400 bytes |
| `0x4488D0` | `Economy_PushHistory` | determinism/sim_patches.cpp | pushes one economy history record |
| `0x333BD0` | `Weather_Roll` | game/weather.cpp | rolls weather (can roll with zero timer) |
| `0x5CFEA0` | `Weather_RoadWetness` | game/weather.cpp | changes road wetness |
| `0x547EF0` | `Network_ClampRoadSpeeds` | determinism/sim_patches.cpp | Late runs at speed 0: clamps road speeds without time passing |
| `0x12EC40` | `Buildings_PeriodicNearCheck` | determinism/sim_patches.cpp | counts world+11f80 every frame, every 20th checks |
| `0x12E6D0` | `Buildings_CheckNear` | determinism/sim_patches.cpp | checks one building when a network/building changes near it |
| `0x12EBA0` | `Buildings_CheckNearAll` | determinism/sim_patches.cpp | checks the queued ones |
| `0x12EAB0` | `Buildings_QueueNearCheck` | determinism/sim_patches.cpp | queues buildings in world+11f88 (+ca0 = 1) |
| `0x12FD80` | `Buildings_DrainQueue` | determinism/sim_patches.cpp | drains world+11fa0, budget 1000 per call; also on save path 42cd57 |
| `0x40ECE0` | `GroundPaint_Drain` | determinism/sim_patches.cpp | drains ground-paint queue world+130d0 (3155f4) |
| `0x40EAA0` | `GroundPaint_Queue` | determinism/sim_patches.cpp | fills world+130d0 when roads/buildings are built |
| `0x47D320` | `Render_ScheduleNext` | determinism/sim_patches.cpp | main loop schedules next render at frame end |
| `0x47D1F0` | `Render_Events` | docs/findings.md | RenderStart/RenderFinished events [[995268]+60] |
| `0x482790` | `Render_Scene` | docs/findings.md | scene |
| `0x9DC80` | `Render_MainThreadAlone` | docs/findings.md | main thread draws alone with byte [99528B] on |
| `0x2593D0` | `Worker_Filling` | determinism/held_frame.cpp | 'Filling' worker; draws world+12810 through 3fa850 |
| `0x3FAB10` | `Early_FillVehicleList` | determinism/determinism.cpp | Early's worker fills vehicle list world+12828 |
| `0x3D85A0` | `View_Prepare` | determinism/sim_patches.cpp | this PC's view preparation (camera object 14250) |
| `0x2F0F10` | `CursorPick` | game/world.h | the game's cursor pick (called by frame loop at 105f39) |
| `0x420350` | `Search_Chooser` | determinism/sim_patches.cpp | picks from search results; draws a random number |
| `0x424430` | `Search_Run` | determinism/sim_patches.cpp | search routine; work/results in three shared vectors |
| `0x256080` | `Citizen_Update` | determinism/sim_patches.cpp | citizen update; scales by min(1, population/K) |
| `0x16E3B0` | `Citizen_TargetsTick` | determinism/sim_patches.cpp | -> 82e590 citizen targets every third tick |
| `0x82E590` | `Workers_Target` | determinism/sim_patches.cpp | building workers' targeting (from 1389e2) |
| `0x824860` | `Citizen_JobSearch` | determinism/citizen_trace.cpp | job search (traced) |
| `0x831120` | `Citizen_Board` | determinism/citizen_trace.cpp | boarding (traced) |
| `0x835D70` | `Citizen_Remove` | determinism/citizen_trace.cpp | citizen removal (traced) |
| `0x823470` | `Citizen_Create` | lockstep/immigrant_invite.cpp | (world, building, partner, bloc, a5, a6) |
| `0x8244F0` | `Citizen_Init` | game/session_patches.cpp | (owner, citizen): sets citizen +18 if not above 0 |
| `0x2FD2E0` | `Citizen_InitCaller` | game/session_patches.cpp | calls Citizen_Init gated at 2fd517 |
| `0x830820` | `Citizen_Retarget` | lockstep/car_owners.h | re-targets a citizen |
| `0x831C50` | `CarOwner_Clear` | lockstep/car_owners.cpp | (world, owner) |
| `0x832AE0` | `Agents_Split` | determinism/sim_patches.cpp | agents' split: every-frame list and three buckets |
| `0x832E90` | `Agents_AdvanceNearCamera` | docs/findings.md | frame only advances near local camera |
| `0x83CEA0` | `Agents_Draw` | determinism/held_frame.cpp | agents for drawing |
| `0x83A6D0` | `Activity_Complete` | determinism/sim_patches.cpp | completion adds done fraction * effects |
| `0x67E6B0` | `Vehicle_EasedRandom` | determinism/state_hash.h | vehicle +1778 eased toward a random number |
| `0x66ADE0` | `Vehicle_FieldWork` | determinism/sim_patches.cpp | field work; speed measured only on this PC's camera |
| `0x66D530` | `Vehicle_Update` | docs/findings.md | vehicle update -> 68b330 |
| `0x6BC9A0` | `Vehicle_SimPass` | determinism/held_frame.cpp | vehicle simulation pass |
| `0x6A7520` | `Vehicle_WagonFollowHead` | determinism/sim_patches.cpp | wagon in a building follows its head; sound levels |
| `0x138B90` | `Building_StepDoors` | determinism/held_frame.cpp | (building, seconds) steps door opening +1238/+1250 |
| `0x16C900` | `Building_Animate` | determinism/sim_patches.cpp | animation phase +1344 near the camera |
| `0x159070` | `Site_StageIndex` | determinism/construction_view.cpp | construction site stage index (+634 cache) |
| `0x167540` | `Site_WriteScaffold` | determinism/construction_view.cpp | writes slot data +20b4..+20cc, scaffold vectors +640/+658/+670 |
| `0x2DC470` | `Site_DrawStages` | determinism/construction_view.cpp | draws construction stages; clears/refills +6b8/+6d0 |
| `0x2DC3C0` | `Site_DrawRoutine` | determinism/construction_view.h | construction drawing routine from 317db0 |
| `0x137B20` | `Late_CopySiteVectors` | determinism/construction_view.cpp | Late copies the drawer's vectors into simulation |
| `0x164BB0` | `Building_Destroy` | determinism/site_watch.cpp | building destructor |
| `0x25BAB0` | `Building_Construct` | determinism/site_watch.cpp | building constructor (0x1420 allocation) |
| `0x168730` | `Building_Demolish` | world/demolish_hook.h | building demolition; hooked inline |
| `0x1A48C0` | `Building_Cost` | determinism/sim_patches.cpp | construction cost (CostFn) |
| `0x191950` | `Building_Refresh` | world/networks.cpp | (thisPtr, building) |
| `0x2F8E10` | `Sapling_TimerRender` | determinism/sim_patches.cpp | saplings from render thread: gives timer 0.001 |
| `0x2F8F20` | `Sapling_GrowLate` | determinism/sim_patches.cpp | sapling growth every tick, queues forest add |
| `0x4FC040` | `Net_DragStart` | world/networks.cpp | drag start; hooked inline; footpath copies 9E4590/9E45A0 into manager+360/+370 |
| `0x4FCAF0` | `Net_DragTick` | world/networks.cpp | drag tick / planner update |
| `0x518A80` | `Net_DragCommit` | world/local_builds.cpp | drag tool commit; hooked inline |
| `0x542A30` | `Net_ManagerResetPreview` | world/networks.cpp | (manager, manager+408): clears tool preview state |
| `0x5434C0` | `Net_RemoveEdge` | lockstep/cancel_plans.cpp | (network, edge, 0, -1, 0) |
| `0x5258E0` | `Net_PlanStart` | world/networks.cpp | (network, dollars, rubles, count, group): start planned construction |
| `0x525C30` | `Net_PlanStartExcept` | world/networks.cpp | (network, dollars, rubles, -, group) |
| `0x5247F0` | `Net_StartEdgeConstruction` | world/networks.cpp | starts an appended edge's construction |
| `0x3AE120` | `Terrain_ClearUnderEdge` | world/networks.cpp | clears the ground under a new edge |
| `0xFF950` | `Net_NodeCtor` | world/networks.cpp | network node constructor (returns the node) |
| `0x21D2B0` | `Vec24_Assign` | world/networks.cpp | std::vector<24-byte record>::operator= |
| `0x21C110` | `Tool_GroupSuspendConst` | world/networks.cpp | group construction tools (group_suspend_const...); hooked inline |
| `0x2BADE0` | `Building_Place` | world/local_builds.cpp | building placement; hooked inline |
| `0x569D70` | `Placement_CollisionQuery` | world/placement_collision.h | placement collision query; hooked inline |
| `0x3A5930` | `Brush_ObstacleTest` | world/placement_collision.cpp | per-point obstacle test |
| `0x3C4850` | `Net_PickHovered` | determinism/sim_patches.cpp | network pick: hovered node or edge |
| `0x3D4B40` | `Net_PickRoad` | determinism/sim_patches.cpp | road pick for access road |
| `0x3B6510` | `Building_ConnectionPort` | determinism/sim_patches.cpp | connection port (world+f6a0) |
| `0x4A12E0` | `UI_BuildingConnPoints` | determinism/sim_patches.cpp | interface draws/picks buildings' connection points |
| `0x592990` | `Water_Refresh` | lockstep/cancel_plans.cpp | 592990([world+fa38], 0) |
| `0x12D990` | `Area_A` | lockstep/cancel_plans.cpp | (world, min, max) |
| `0x12F010` | `Area_B` | lockstep/cancel_plans.cpp | (world, min, max) |
| `0x53E630` | `Net_JoinEdges` | lockstep/join_tool.h | joins two edges |
| `0x523C70` | `Net_VehiclesGoHome` | lockstep/call_home.cpp | (network, object, building): parked vehicles go home |
| `0x3D00F0` | `Tool_Road` | world/networks.cpp | ROAD wrapper (road_*) |
| `0x3B0E60` | `Tool_Pipe` | world/networks.cpp | PIPE wrapper |
| `0x3B12B0` | `Tool_WaterPipe` | world/networks.cpp | WATERPIPE wrapper |
| `0x3B4230` | `Tool_HeatPipe` | world/networks.cpp | HEATPIPE wrapper |
| `0x3D4730` | `Tool_Rail` | world/networks.cpp | RAIL wrapper |
| `0x3D68B0` | `Tool_Pedestrian` | world/networks.cpp | PEDESTRIAN (footpath) wrapper: pedestrianway_* |
| `0x3B3820` | `Tool_Fence` | world/networks.cpp | FENCE wrapper |
| `0x3B8780` | `Tool_PowerHigh` | world/networks.cpp | POWER_HIGH wrapper |
| `0x3BAED0` | `Tool_PowerLow` | world/networks.cpp | POWER_LOW wrapper |
| `0x3AEC30` | `Tool_Conveyor` | world/networks.cpp | CONVEYOR wrapper |
| `0x3CBFA0` | `Tool_Cableway` | world/networks.cpp | CABLEWAY wrapper |
| `0x3C9130` | `Tool_Airroad` | world/networks.cpp | AIRROAD wrapper |
| `0x3D6E70` | `Tool_FactoryConn` | world/networks.cpp | FACTORYCONN wrapper |
| `0x3CF6F0` | `Tool_SecondRoad` | world/networks.cpp | SECOND_ROAD wrapper (pavements, second_road_*) |
| `0x214220` | `Tool_CancelHandler` | lockstep/cancel_tool.h | handler every frame the tool is selected |
| `0x243570` | `Tool_RandomSelectedBuilding` | determinism/sim_patches.cpp | Late tool stub |
| `0x2F94C0` | `Tool_Tree` | lockstep/brush_tools.cpp | BrushTools_Tree |
| `0x3A8A70` | `Tool_TerrainBrush` | lockstep/brush_tools.cpp | terrain_level/raise/lower (BrushTools_Terrain) |
| `0x3ABC90` | `Tool_TerrainDemolish` | determinism/sim_patches.cpp | terrain_demolish |
| `0x3ADF80` | `Tool_DemolishTrees` | lockstep/brush_tools.cpp | terrain_demolish_trees |
| `0x3CCD70` | `Tool_RestoreTopsoil` | lockstep/brush_tools.cpp | terrain_restore_topsoil |
| `0x32D240` | `Tool_NamePoint` | lockstep/new_city.h | city tool handler (namepoint) |
| `0x234A50` | `Tool_TerrainEditor` | determinism/sim_patches.cpp | terrain_*_editor |
| `0x235140` | `Tool_PaintResource` | determinism/sim_patches.cpp | paint/erase coal, iron, oil, bauxite, uranium |
| `0x235370` | `Tool_PaintRock` | determinism/sim_patches.cpp | paint/erase rock |
| `0x235580` | `Tool_PaintOasis` | determinism/sim_patches.cpp | paint/erase oasis |
| `0x238310` | `Tool_PaintTrees` | determinism/sim_patches.cpp | paint/erase trees |
| `0x417A70` | `Tool_Waypoint` | lockstep/hover_tools.cpp | road_waypoint |
| `0x417D80` | `Tool_Sign` | lockstep/hover_tools.cpp | road sign |
| `0x4181D0` | `Tool_SignErase` | lockstep/hover_tools.cpp | road_sign_erase |
| `0x4182D0` | `Tool_Junction` | lockstep/hover_tools.cpp | junction tool |
| `0x4184C0` | `Tool_RailPlace` | lockstep/hover_tools.cpp | rail semaphore place |
| `0x418F90` | `Tool_RailRemove` | lockstep/hover_tools.cpp | rail_semaphorecancel |
| `0x3C9A00` | `Tool_JoinHub` | lockstep/join_tool.cpp | join_hub_all (JoinTool_Run) |
| `0x3C85B0` | `Tool_Buoy` | lockstep/buoy_tool.cpp | bojka_* (BuoyTool_Run) |
| `0x3CA560` | `Tool_GoThere` | lockstep/go_there.cpp | terrain_go_there (GoThere_Run) |
| `0x5F78D0` | `Tool_Measurement` | determinism/sim_patches.cpp | measurement |
| `0x5EEDF0` | `Tool_AssignCO` | lockstep/office_selection.h | area office tool (assign_co) |
| `0x216780` | `Tool_AreaCopy` | determinism/sim_patches.cpp | area_copy |
| `0x216970` | `Tool_AreaPaste` | determinism/sim_patches.cpp | area_paste |
| `0x2B23B0` | `Tool_CrossroadAdd` | determinism/sim_patches.cpp | crossroad_add |
| `0x2B2760` | `Tool_VehicleAddStop` | lockstep/route_tools.cpp | vehicle_addstop |
| `0x2B18C0` | `Tool_VehicleAddStopCopy` | determinism/sim_patches.cpp | vehicle_addstop_copy |
| `0x2B2DD0` | `Tool_VehicleGoDepot` | lockstep/route_tools.cpp | vehicle_godepo (RouteTools_DepotTool) |
| `0x2B3C80` | `Tool_VehicleReplace` | determinism/sim_patches.cpp | vehicle_replace |
| `0x2B4680` | `Tool_VehicleReplaceAuto` | determinism/sim_patches.cpp | vehicle_replace_auto |
| `0x2B4A50` | `Tool_LineAddStop` | lockstep/route_tools.cpp | line_addstop |
| `0x2B4C60` | `Tool_LineCopy` | lockstep/route_tools.cpp | RouteTools_LineCopyTool |
| `0x2B6AB0` | `Tool_VehicleToVehicle` | lockstep/game_calls.cpp | vehicle_tovehicle |
| `0x2B6F30` | `Tool_AddStation` | lockstep/link_tools.cpp | building_addstation |
| `0x2B78B0` | `Tool_AddStationDist` | lockstep/link_tools.cpp | building_addstation_dist |
| `0x2B8780` | `Tool_CopyDist` | determinism/sim_patches.cpp | building_copy_dist |
| `0x1D48A0` | `Tool_FireAdd` | lockstep/link_tools.cpp | firestation_addfire |
| `0x1D51A0` | `Tool_AddSource` | lockstep/source_slots.cpp | building_addsource |
| `0x1D6820` | `Tool_CopySource` | determinism/sim_patches.cpp | building_copysource |
| `0x1D6CB0` | `Tool_CopyConstruction` | lockstep/game_calls.cpp | building_copyconstruction |
| `0x1D71C0` | `Tool_CopyWeights` | lockstep/link_tools.cpp | building_copyweights |
| `0x1D8410` | `Tool_AddField` | determinism/sim_patches.cpp | building_addfield |
| `0x1D88A0` | `Tool_SendConstruction` | determinism/sim_patches.cpp | vehicle_sendconstruction |
| `0x1D9980` | `Tool_AddConstruction` | lockstep/game_calls.cpp | building_addconstruction |
| `0x1DA580` | `Tool_AddDemolition` | lockstep/game_calls.cpp | building_adddemolition |
| `0x1DB1B0` | `Tool_Distribution` | determinism/sim_patches.cpp | building_distribution |
| `0x1DB9E0` | `Tool_CopyWaste` | determinism/sim_patches.cpp | building_copy_waste |
| `0x1DBD40` | `Tool_CopyTransfer` | determinism/sim_patches.cpp | building_copy_transfer |
| `0x2B8CE0` | `Tool_2B8CE0` | lockstep/link_tools.cpp | worker_relocate* (LinkTools_Relocate) |
| `0x213870` | `Tool_PickFromMap_A` | determinism/sim_patches.cpp | pick_tool_from_map |
| `0x214530` | `Tool_PickFromMap_B` | determinism/sim_patches.cpp | pick_tool_from_map |
| `0x3CB0D0` | `Tool_PickFromMap_C` | determinism/sim_patches.cpp | pick_tool_from_map |
| `0x3CE1B0` | `Tool_3CE1B0` | determinism/sim_patches.cpp | Late tool stub target |
| `0x3CE5F0` | `Tool_3CE5F0` | determinism/sim_patches.cpp | Late tool stub target |
| `0x3CE650` | `Tool_3CE650` | determinism/sim_patches.cpp | Late tool stub target |
| `0x2B5120` | `Route_StopPick` | lockstep/route_tools.cpp | stop pick of the route tools |
| `0x40BD60` | `Tool_MarkList` | lockstep/link_tools.cpp | depot/link tools' mark |
| `0x40CDE0` | `Tool_MarkLine` | lockstep/route_tools.cpp | line tool's mark (2b505c) |
| `0x3FBDC0` | `Vehicle_SendToDepot` | lockstep/game_calls.cpp | (world, vehicle, 0,0,1,1); also call home |
| `0x3FB8E0` | `Vehicle_SendToWorkplace` | lockstep/game_calls.cpp | send back to workplace |
| `0x3FDEF0` | `Vehicle_Uncouple` | lockstep/game_calls.cpp | (world, vehicle) takes dragged vehicle off its head |
| `0x3FD5D0` | `Vehicle_Couple` | lockstep/game_calls.cpp | (world, head, vehicle, index, 1, 0, &result) |
| `0x3FE520` | `Vehicle_AttachTrain` | lockstep/game_calls.cpp | (-, head) |
| `0x3FFAD0` | `Sound_Play` | lockstep/game_calls.cpp | refused sound (WIb) |
| `0x3DEE50` | `Vehicle_QueueSale` | lockstep/game_calls.cpp | (world, vehicle, 1,0,0,0, 0.0f) |
| `0x3BB330` | `Vehicle_Refresh` | lockstep/game_calls.cpp | (world, vehicle) |
| `0x3BB640` | `Vehicle_LinkedToDepot` | lockstep/route_tools.cpp | LinkedFn |
| `0x1AA540` | `Vehicle_AtHome` | lockstep/route_tools.cpp | line home test |
| `0x1AE130` | `Vehicle_SetRoute` | lockstep/route_tools.cpp | (world, route, v, 0) |
| `0x1AAAA0` | `Vehicle_Replace` | lockstep/game_calls.cpp | replace vehicle |
| `0x1AB290` | `Vehicle_PrepareReplace` | lockstep/game_calls.cpp | prepare the replaced vehicle |
| `0x1AAD70` | `Vehicle_ImportPurchase` | docs/findings.md | import purchase |
| `0x3A38F0` | `Depot_FreeSlot` | lockstep/route_tools.cpp | SlotFn(0, depot) |
| `0x3A3830` | `Depot_SlotFits` | lockstep/route_tools.cpp | SlotFitsFn |
| `0x3E2900` | `Depot_TypeAllowed` | lockstep/route_tools.cpp | DepotTypeFn |
| `0x3E3F40` | `Depot_TypeSlot` | lockstep/purchase.cpp | (world, depot, type, 0, &slot) |
| `0x3E4A00` | `Vehicle_Create` | lockstep/purchase.cpp | CreateFn(world, type, building, ...) |
| `0x3EA250` | `VehicleType_LoadMaterial` | lockstep/purchase.cpp | (-, type, variant) |
| `0x3EA7D0` | `VehicleType_Load` | lockstep/purchase.cpp | (world, type) |
| `0x627E80` | `Purchase_CampaignAllowed` | lockstep/purchase.cpp | (world, depot, typeName) |
| `0x76A170` | `Purchase_RefillWindow` | lockstep/purchase.cpp | (world, window) |
| `0x660A70` | `UsedMarket_Apply` | lockstep/purchase.cpp | (record, vehicle) |
| `0x6624D0` | `UsedMarket_Remove` | lockstep/purchase.cpp | (market, type) |
| `0x2AB810` | `Depot_SlotDone` | lockstep/purchase.cpp | (world, depot, slotRecord) |
| `0x33F0` | `List_AddVehicle` | lockstep/purchase.cpp | AddToSlotFn(list, vehicle) |
| `0x68B2B0` | `Vehicle_Unusable` | lockstep/route_tools.cpp | UnusableFn |
| `0x6B7070` | `Vehicle_SendTo` | lockstep/route_tools.cpp | SendFn(v, depot) |
| `0x6B7420` | `Vehicle_SendResult` | lockstep/route_tools.cpp | SendResultFn |
| `0x6DE260` | `Vehicle_Unload` | lockstep/route_tools.cpp | UnloadFn |
| `0x6B7670` | `Vehicle_ToggleState` | lockstep/game_calls.cpp | toggle vehicle state |
| `0x6BF1D0` | `Vehicle_RebuildRoute` | lockstep/game_calls.cpp | rebuild route / cargo summary (border refresh) |
| `0x6C52C0` | `Vehicle_Release` | lockstep/game_calls.cpp | release vehicle |
| `0x6710B0` | `Vehicle_NextSkin` | lockstep/game_calls.cpp | next color/skin |
| `0x67CC40` | `Vehicle_RefreshAfterLineClear` | lockstep/game_calls.cpp |  |
| `0x67D5D0` | `Vehicle_AddBorderStop` | lockstep/game_calls.cpp | stop beyond the borders |
| `0x68A420` | `Vehicle_LoadOnto` | lockstep/game_calls.cpp | (picked vehicle, window vehicle, 1) |
| `0x6BEEE0` | `Storage_Empty` | lockstep/game_calls.cpp | empties building storage or wagon cargo |
| `0x6C2430` | `Vehicle_Readiness` | lockstep/link_tools.cpp | fire tool's test |
| `0x2582E0` | `Fire_DispatchVehicle` | lockstep/link_tools.cpp | a vehicle sent to a fire |
| `0x6147E0` | `AutoReplace_Locomotive` | lockstep/game_calls.cpp | locomotive back to auto-replacement |
| `0x614840` | `AutoReplace_SetType` | lockstep/game_calls.cpp | set/clear the type's auto-replacement |
| `0x6EE2A0` | `Message_Show` | lockstep/game_calls.cpp | refused message |
| `0x491500` | `Line_SpacingOn` | lockstep/line_spacing.cpp | (line): spacing on, caches length +25c |
| `0x4915B0` | `Line_Clear` | lockstep/game_calls.cpp | clear line data |
| `0x4945A0` | `Lines_MergeEqual` | lockstep/line_accept.cpp | merge equal lines |
| `0x495830` | `Line_AcceptCopy` | lockstep/line_accept.cpp | accept changes |
| `0x4962C0` | `Lines_DeleteCleanup` | lockstep/game_calls.cpp | (manager, 1) |
| `0x4963D0` | `Lines_RenameAtBuilding` | lockstep/game_calls.cpp | lines at renamed building take its name |
| `0x496DE0` | `Lines_Create` | lockstep/game_calls.cpp | create new line |
| `0x497650` | `Line_VehiclesTakeStops` | lockstep/line_accept.cpp |  |
| `0x497B10` | `Line_AssignVehicle` | lockstep/game_calls.cpp | copy schedule to / clear vehicle's line |
| `0x498990` | `Line_Test` | lockstep/route_tools.cpp |  |
| `0x499600` | `Line_NamePrivate` | lockstep/line_accept.cpp | name a private line / delete |
| `0x499790` | `Lines_Refresh` | lockstep/cargo_settings.cpp | refresh line manager for a vehicle |
| `0x22A6D0` | `MarkChanged` | lockstep/junction_accept.cpp | mark line/junction changed |
| `0x224300` | `Junction_Reset` | lockstep/junction_accept.cpp |  |
| `0x226530` | `Junction_Copy` | lockstep/junction_accept.cpp | copy into the junction |
| `0xF98D0` | `Junction_Unmark` | lockstep/junction_accept.cpp | take the junction off the list |
| `0x228B50` | `Junctions_RebuildList` | determinism/held_frame.cpp | (junction manager [world+12808]) |
| `0x2239D0` | `Junction_Queue` | determinism/held_frame.cpp | queues each one (rule +14451) |
| `0x222A50` | `Junction_MakeWorkingCopy` | lockstep/junction_accept.h |  |
| `0x160550` | `Building_SetPay` | lockstep/building_settings.cpp | (world, building, rubles, dollars) |
| `0x160570` | `Building_Stop` | lockstep/building_settings.cpp | (world, building, 1) / continue construction |
| `0x161440` | `Building_Resume` | lockstep/building_settings.cpp | (world, building) |
| `0x1754D0` | `Building_SetAutoPurchase` | lockstep/building_settings.cpp | (world, bld, value) |
| `0x1B7AF0` | `Pipeline_ToExport` | lockstep/building_settings.cpp | empties export slots |
| `0x1B7C00` | `Pipeline_ToImport` | lockstep/building_settings.cpp | fills chosen resource |
| `0x161C90` | `Office_Assign` | lockstep/game_calls.cpp | assign construction office (WOOFbb) |
| `0x162360` | `Office_Unassign` | lockstep/game_calls.cpp | (world, office, building) |
| `0x162100` | `Farm_AddField` | lockstep/game_calls.cpp |  |
| `0x162A30` | `Links_ClearAll` | lockstep/game_calls.cpp |  |
| `0x162B40` | `Links_RemoveByIndex` | lockstep/game_calls.cpp |  |
| `0x162BD0` | `Links_RemoveToPartner` | lockstep/game_calls.cpp |  |
| `0x1C7E80` | `Links_RemoveFromPartner` | lockstep/game_calls.cpp |  |
| `0x1512A0` | `LinkRecord_Ctor` | lockstep/link_tools.cpp |  |
| `0x76A3A0` | `Links_Shares` | lockstep/link_tools.cpp | load/unload shares from the source's links |
| `0x1561C0` | `Office_AddConstruction` | lockstep/game_calls.cpp | (world, 0, office, resource mode, byte) |
| `0x1A8D70` | `Building_StartReconstruction` | lockstep/game_calls.cpp |  |
| `0x1A04F0` | `Office_RebuildConstructionList` | lockstep/game_calls.cpp |  |
| `0x19F5F0` | `Office_Lookup` | determinism/office_guard.cpp | building +da8 -> office +c70 -> vehicle +1708 |
| `0x1AC720` | `Building_RecountWorkerVehicles` | lockstep/game_calls.cpp | recounts +ff0 records from +df0 |
| `0x19D3B0` | `Waste_AddContainer` | lockstep/game_calls.cpp |  |
| `0x19D8B0` | `Waste_RemoveContainer` | lockstep/game_calls.cpp |  |
| `0x1D4330` | `Destinations_Add` | lockstep/game_calls.cpp |  |
| `0x1D4440` | `Destinations_Remove` | lockstep/game_calls.cpp |  |
| `0x1D4550` | `Destinations_Share` | lockstep/game_calls.cpp | (-, entry, delta, list) |
| `0x1D7E10` | `Sources_Copy` | lockstep/game_calls.cpp | copy sources (vector<bool> world+f5c8) |
| `0x1DC5D0` | `Building_CopyWaste` | lockstep/game_calls.cpp |  |
| `0x1DC7D0` | `Building_CopyTransfer` | lockstep/game_calls.cpp |  |
| `0x2B8B10` | `Building_CopyDistLinks` | lockstep/game_calls.cpp |  |
| `0x1EA1A0` | `Building_SendVehiclesWithCargo` | lockstep/game_calls.cpp |  |
| `0x1EA2A0` | `Building_SendVehiclesWithCargo2` | lockstep/game_calls.cpp |  |
| `0x1513F0` | `CargoSettings_Destroy` | lockstep/cargo_settings.cpp | half destroy |
| `0x204EF0` | `CargoSettings_Copy` | lockstep/cargo_settings.cpp |  |
| `0x2571E0` | `Crime_AdvanceCase` | lockstep/crime_cancel.cpp | (world, case, float) |
| `0x2580D0` | `Crime_CloseCase` | lockstep/crime_cancel.cpp | (world, case) |
| `0x5F2A20` | `Office_SaveSelection` | lockstep/office_selection.cpp | saved with the city |
| `0x5EDC30` | `Office_5EDC30` | lockstep/office_selection.cpp | checked by prologue |
| `0x2AA830` | `Resource_ByName` | lockstep/brush_tools.cpp | (world, name) -> resource definition |
| `0x2FDB00` | `Stats_Add` | lockstep/brush_tools.cpp | (world, {resource, qty, money}, list) |
| `0x4B9140` | `Loan_Borrow` | lockstep/game_calls.cpp | (world, currency, duration) |
| `0x337170` | `Settings_Apply` | lockstep/commands.cpp | (world) game rules |
| `0x337450` | `Settings_SystemTab` | game/system_menu.cpp | draws the system tab / money button |
| `0x32F450` | `City_Create` | lockstep/new_city.cpp | city tool's new city |
| `0x32FF40` | `City_AssignBuildings` | lockstep/new_city.cpp |  |
| `0x32FA50` | `City_DrawLabels` | lockstep/rename.cpp |  |
| `0x330690` | `City_RandomName` | lockstep/rename.cpp | named with a random entry |
| `0x32D150` | `City_NameFree` | lockstep/new_city.h |  |
| `0x261210` | `MainMenu_Draw` | ui/multiplayer_page.cpp | (world) |
| `0x274E70` | `MainMenu_DrawButtons` | game/main_menu_hook.cpp | sets click at 27533e |
| `0x28AF60` | `MainMenu_BuildRoot` | game/main_menu_hook.cpp | root menu builder; hooked inline |
| `0x4BDE80` | `Minimap_DrawMapAndBorder` | game/minimap.cpp | hooked inline |
| `0x4BEB10` | `Minimap_Draw` | game/minimap.h |  |
| `0x4C1370` | `Minimap_Panel` | game/minimap.h |  |
| `0x410D50` | `Border_IsOutside` | modes/republics/republics.cpp | bool (world, xyz): outside the playable area |
| `0x40F3F0` | `Border_CutLine` | modes/republics/republics.cpp |  |
| `0x410080` | `Border_Signs` | modes/republics/republics.cpp |  |
| `0x411820` | `Border_Draw` | game/border_draw.cpp |  |
| `0x411ED0` | `Border_DrawInset` | game/border_draw.cpp |  |
| `0x3D7AB0` | `Camera_Control` | game/camera.h |  |
| `0x403F70` | `Camera_Projection` | game/camera.h |  |
| `0x6F8CC0` | `Window_Building` | determinism/sim_patches.cpp |  |
| `0x7FC880` | `Window_Building2` | determinism/sim_patches.cpp |  |
| `0x80CC60` | `Window_Purchase` | lockstep/purchase.h | Buy press read at 810943 |
| `0x801300` | `Window_Rename` | lockstep/rename.h |  |
| `0x758420` | `Window_Junction` | lockstep/junction_accept.h |  |
| `0x7CFF50` | `Window_Line` | lockstep/line_spacing.h |  |
| `0x7A1610` | `Window_CarOwners` | lockstep/car_owners.h |  |
| `0x794FB0` | `Window_Factory` | lockstep/plan_remove.h | production queue |
| `0x7677C0` | `Window_CrimeCases` | lockstep/crime_cancel.cpp | police/court cases |
| `0x2F7ED0` | `Window_Research` | lockstep/research.h |  |
| `0x776D40` | `Panel_Destinations` | lockstep/game_calls.cpp |  |
| `0x7EC380` | `Window_HalfEditor` | lockstep/cargo_settings.h |  |
| `0x7F2D50` | `Window_SellConfirm` | lockstep/game_calls.cpp |  |
| `0x745160` | `Popup_LinkOrder` | lockstep/popups.cpp |  |
| `0x770640` | `Popup_OfficeOrder` | lockstep/popups.cpp |  |
| `0x778490` | `Popup_DestinationsPercent` | lockstep/popups.cpp |  |
| `0x799550` | `Production_Next` | lockstep/popups.cpp |  |
| `0x3BD50` | `Popup_BuildValues` | lockstep/popups.h |  |
| `0x42BA0` | `Popup_DestroyValues` | lockstep/popups.cpp |  |
| `0xFF520` | `Widget_SetFloat` | lockstep/world_workers.cpp |  |
| `0xFF6A0` | `Widget_GetFloat` | lockstep/world_workers.cpp |  |
| `0x8E050` | `VecPtr_PushBack` | lockstep/link_tools.cpp | vector<T*>::push_back |
| `0xB0E90` | `Vec_Push` | lockstep/purchase.cpp | PushFn(vector, &value) |
| `0xB1700` | `Vec_Reserve` | lockstep/call_home.cpp | (vector, 1) |
| `0xCC880` | `Vec_Grow` | lockstep/purchase.cpp |  |

## Globals (label + comment)

| RVA | Name | Source | Note |
|---|---|---|---|
| `0x9D4F10` | `g_World` | game/world.h | the world / placement god-object |
| `0x9D4EE0` | `g_GameTimer` | determinism/determinism.cpp | C3D_TIMER the frame loop scales |
| `0x9BDFC0` | `g_Config` | lockstep/shared_save.cpp | C3D_CONFIGURATOR: the game's options |
| `0x9E4558` | `Hover_Edge` | world/networks.cpp | network edge under the cursor |
| `0x9E4560` | `Hover_Node` | world/networks.cpp | network node under the cursor (thisPtr+f650) |
| `0x9E4590` | `Hover_RoadEdge` | world/networks.cpp | road segment under the cursor (footpath joins) |
| `0x9E4598` | `Hover_RoadPoint` | world/networks.cpp | int: point the path would split it at |
| `0x9E45A0` | `Hover_RoadNode` | world/networks.cpp | road node under the cursor |
| `0x9E45B0` | `Hover_PortBuilding` | world/networks.cpp | building whose connection port is hovered |
| `0x9E45B8` | `Hover_PortIndex` | world/networks.cpp |  |
| `0x9E45D8` | `Hover_Edge2` | world/networks.cpp | second hover edge |
| `0x9E45E0` | `Hover_Node2` | world/networks.cpp | second hover node |
| `0x9E45E8` | `Hover_Ptr0` | world/networks.cpp | hover pointer |
| `0x9E45F8` | `Hover_Flag` | world/networks.cpp |  |
| `0x9E46B0` | `g_RoadNetwork` | game/world.h | road network manager; 0 with no world |
| `0x9E6A18` | `g_BuildingsBegin` | world/networks.cpp | city buildings vector |
| `0x9E6A20` | `g_BuildingsEnd` | world/networks.cpp |  |
| `0x9E75B8` | `g_People` | determinism/state_hash.h | world+126A8 citizens |
| `0x9E7710` | `g_LinesManager` | lockstep/game_calls.cpp | world+0x12800 (was g_AgentQueue; held_frame reads its +0x60 queue) |
| `0x9E7720` | `g_VehiclesBegin` | lockstep/purchase.cpp | world+0x12810 (was g_SimVec_A) |
| `0x9E7728` | `g_VehiclesEnd` | lockstep/purchase.cpp | world+0x12818 (was g_SimVec_B) |
| `0x9E7738` | `g_EarlyVehicleList` | determinism/determinism.cpp | world+0x12828 (was g_SimVector) |
| `0x9E2350` | `g_VehicleJobs` | determinism/held_frame.cpp | vehicle job queue object / paths |
| `0x9E19D0` | `g_OpenWindows` | lockstep/cargo_settings.cpp |  |
| `0x9E8088` | `g_CampaignOrCurve` | world/networks.cpp | != -1 campaign / preset curve index |
| `0x9E9C90` | `g_Camera` | game/camera.cpp |  |
| `0x9E9D10` | `g_CameraPos` | lockstep/join_tool.cpp | float x,y,z |
| `0x9E9D20` | `g_CameraYaw` | world/networks.cpp |  |
| `0x9EAD10` | `g_CameraMatrices` | game/camera.cpp | view then projection |
| `0x9EACD0` | `g_MiddlePoint` | game/border_draw.cpp | C3D_MIDDLEPOINT |
| `0x9EAD08` | `g_CurrentShaders` | game/panel2d.cpp |  |
| `0x9BE060` | `g_Panel2D` | game/panel2d.cpp | C3D_PANEL2D for the HUD |
| `0x9E0B38` | `g_WhiteTexture` | game/panel2d.cpp |  |
| `0x9E0B40` | `g_MinimapDashTexture` | game/minimap.cpp |  |
| `0x9941F0` | `g_Engine` | determinism/held_frame.cpp | +ff8 models, +ee0 quadtree |
| `0x995268` | `g_Renderer` | determinism/held_frame.cpp | +68 finished event |
| `0x995290` | `g_FillingWorker` | determinism/determinism.cpp |  |
| `0x996FB0` | `g_FontManager` | lockstep/rename.cpp |  |
| `0x996B98` | `g_DepotDragVehicle` | lockstep/game_calls.cpp |  |
| `0x996810` | `g_PowerTimeAccum` | determinism/determinism.cpp |  |
| `0x9962A8` | `g_FrameCounterBuildings` | determinism/determinism.cpp | mod 3 |
| `0x9963F0` | `g_FrameCounterAgents` | determinism/determinism.cpp |  |
| `0x9962B0` | `g_SaveName` | save/mod_save.cpp | char[] save\<name> |
| `0x993C65` | `g_WorldLoaded` | game/world.h | byte |
| `0x993C66` | `g_LoadRequest` | game/save_load.h | byte |
| `0x995289` | `g_ReturnToMenu` | game/save_load.h |  |
| `0x995298` | `g_BuiltinTerrainFlag` | save/mod_save.cpp |  |
| `0x99528A` | `g_InMenu` | docs/findings.md |  |
| `0x99528B` | `g_MainThreadDrawsAlone` | docs/findings.md |  |
| `0x99528C` | `g_ScreenWidth` | game/panel2d.cpp |  |
| `0x995274` | `g_ScreenHeight` | game/panel2d.cpp |  |
| `0x995214` | `g_MenuScale` | game/main_menu_hook.cpp |  |
| `0x992088` | `g_ToolbarScale` | game/system_menu.cpp |  |
| `0x9D54B4` | `g_SpeedShown` | determinism/determinism.cpp |  |
| `0x9D54B8` | `g_SpeedRequested` | determinism/determinism.cpp |  |
| `0x9D54BC` | `g_SpeedFast` | determinism/determinism.cpp |  |
| `0x9D54B2` | `g_SpeedMax` | docs/findings.md |  |
| `0x9D54C0` | `g_GameMode` | lockstep/link_tools.cpp | fire tool reads it |
| `0x9D54CC` | `g_Realism` | determinism/held_frame.cpp |  |
| `0x9D5490` | `g_Dollars` | docs/findings.md | world+580 |
| `0x9D5498` | `g_Rubles` | docs/findings.md |  |
| `0xA54B90` | `g_Input` | game/input_block.cpp | C3D_INPUT key bytes |
| `0xA54BAD` | `g_KeyCtrl` | lockstep/route_tools.cpp |  |
| `0xA54CAC` | `g_KeyEnter` | lockstep/research.cpp |  |
| `0xA54D2C` | `g_KeyKpEnter` | lockstep/research.cpp |  |
| `0xA54C91` | `g_EscapeLatch` | game/input_block.cpp |  |
| `0xA54E90` | `g_LmbHeld` | game/input_block.cpp |  |
| `0xA54E91` | `g_LmbPressed` | game/input_block.cpp | clearing it swallows a click |

## Patch / hook sites (EOL comment only)

| RVA | Name | Source | Note |
|---|---|---|---|
| `0x105DF3` | `FRAME_LOCK_SITE` | determinism/sim_patches.cpp | call [EnterCriticalSection] on renderer+0x80 |
| `0x105E56` | `PREPARE_CALL` | determinism/sim_patches.cpp |  |
| `0x105F39` | `CURSOR_PICK_CALL` | determinism/sim_patches.cpp |  |
| `0x1389F9` | `SITE_UPDATE_CALL` | determinism/sim_patches.cpp |  |
| `0x13CE50` | `GETNODE_SITE` | determinism/sim_patches.cpp |  |
| `0x2144D2` | `REMOVE_CALL` | lockstep/cancel_tool.cpp | the cancel handler's call of it |
| `0x2B3795` | `DEPOT_MARK_RETURN` | lockstep/route_tools.cpp | after the mark's call at 2b3790 |
| `0x2CC37C` | `PLACE_PRESS_SITE` | determinism/sim_patches.cpp |  |
| `0x2FD517` | `CITIZEN_INIT_GATED_CALL` | game/session_patches.cpp | its call in FUN_1402fd2e0 (Late body and interface) |
| `0x30D9B2` | `SAPLING_LATE_CALL` | determinism/sim_patches.cpp |  |
| `0x318D06` | `BORDER_CALL_INSET` | game/border_draw.cpp | call 411ed0 |
| `0x318D0D` | `BORDER_CALL` | game/border_draw.cpp | call 411820 |
| `0x318E7F` | `LABELS_CALL` | lockstep/rename.cpp | its call there (world in rcx) |
| `0x39F2AB` | `PANEL_SITE` | lockstep/cancel_plans.cpp | call [GetMouseLeftPress] (6 bytes): the network panel |
| `0x418C9B` | `TRAINS_SIDE_READ` | session/save_sync.cpp |  |
| `0x482B52` | `UI_ROUTINE_CALL` | determinism/sim_patches.cpp |  |
| `0x6FEDC0` | `PARTNER_DRAW_SITE` | lockstep/immigrant_invite.cpp |  |
| `0x6FF9F0` | `EDUCATION_DRAW_SITE` | lockstep/immigrant_invite.cpp |  |
| `0x701A2C` | `PRESS_SITE` | lockstep/call_home.cpp | the window's call [import] (6 bytes) |
| `0x701A49` | `BLOCK_BEGIN` | lockstep/call_home.cpp | the inline move (checked by a hash) |
| `0x701D20` | `BLOCK_END` | lockstep/call_home.cpp |  |
| `0x768536` | `TEXT_SITE` | lockstep/crime_cancel.cpp | mov edx,0x986 ("Cancel assignment") |
| `0x768570` | `BLOCK_SITE` | lockstep/crime_cancel.cpp | the click test, its je last |
| `0x768584` | `JE_SITE` | lockstep/crime_cancel.cpp | je 768681 (6 bytes) |
| `0x76858A` | `NATIVE_RVA` | lockstep/crime_cancel.cpp | after the je: the hover and acted tests, then the effect |
| `0x798CC2` | `PRESS_RVA` | lockstep/plan_remove.cpp | the press test, start of the checked block (0x6a bytes) |
| `0x798CCA` | `CLEAR_RVA` | lockstep/plan_remove.cpp | mov [a54e91],cl (6 bytes) |
| `0x798CD5` | `ERASE_RVA` | lockstep/plan_remove.cpp |  |
| `0x798CFB` | `DONE_RVA` | lockstep/plan_remove.cpp |  |
| `0x798D2C` | `SKIP_RVA` | lockstep/plan_remove.cpp |  |
| `0x7A1A17` | `BLOCK_BEGIN` | lockstep/car_owners.cpp | the press and its loop (checked by a hash) |
| `0x7A1A88` | `CLEAR_SITE` | lockstep/car_owners.cpp |  |
| `0x7A1AAB` | `HOME_SITE` | lockstep/car_owners.cpp |  |
| `0x7A1ACB` | `BLOCK_END` | lockstep/car_owners.cpp |  |
| `0x7D3C71` | `PRESS_RVA` | lockstep/line_spacing.cpp | the button's press test, start of the checked block |
| `0x7D3C8C` | `PATCH_RVA` | lockstep/line_spacing.cpp | lea rdx,[click sound] (7 bytes) |
| `0x7D3C93` | `BACK_RVA` | lockstep/line_spacing.cpp |  |
| `0x7D3D06` | `BLOCK_END` | lockstep/line_spacing.cpp |  |
| `0x810943` | `BUY_PRESS_SITE` | determinism/sim_patches.cpp |  |
| `0x810949` | `PRESS_RETURN_RVA` | lockstep/purchase.cpp | after the relayed call at 810943 |
