// Hover tips for every row of the debug menu (included by settings.cpp, inside its namespace).
// Rules (user 2026-10-23): plain words, what it does and what On/Off or more/less changes, one or two sentences,
// "next start" / "first person only" said where true. A row's tip shows under it after it stayed picked for a
// moment (kTipDelay), by laser or stick. Keys are "section.key" of the ini, so a row keeps its tip wherever it sits.
struct TipText {
    const char* key;
    const wchar_t* tip;
};
const TipText kTips[] = {
    // Play and Options front
    {"ui.first_person", L"What the game's own menus (build/buy, panels, wrist buttons) show in first person: nothing, the wrist HUD, the game's menu in front of you, or both."},
    // Graphics > Picture
    {"stereo.render_scale", L"How sharp the picture is before it goes to the headset. Higher is sharper but needs a stronger graphics card; lower is faster."},
    {"stereo.sharpen", L"Adds crispness to the picture. 0% is off; too much makes edges look rough."},
    {"stereo.texture_bias", L"Makes textures (floors, clothes) look sharper from a distance. 0 is the game's own. Takes effect next start."},
    {"stereo.upscale", L"On: the game draws smaller and AMD FSR scales it up to the headset size. Off: the game draws at Render resolution and is only sharpened."},
    {"stereo.fsr_render_scale", L"With FSR on: how big the game draws before FSR scales it up. Lower is faster but softer."},
    {"stereo.fsr_output_scale", L"With FSR on: the size of the picture sent to the headset. Higher is sharper but costs more."},
    {"stereo.gpu_yield", L"Lets the headset stream get the graphics card first when both compete. Try it if the picture stutters in the headset only."},
    {"stereo.align_eyes", L"SteamVR only: lines both eyes up with what the headset reports. Leave on unless the picture looks cross-eyed."},
    {"stereo.exact_fov", L"Each eye draws only the part the lens shows, which is up to a third less picture to draw. Turn off if the view centre looks wrong."},
    {"stereo.ambient_occlusion", L"Soft shading in corners and under objects. Pretty, but costs speed. Takes effect next start."},
    // Graphics > Mirrors
    {"stereo.mirror_far_inside", L"How far mirrors show things indoors. Shorter is faster. 0 uses the game's value. Takes effect next start."},
    {"stereo.mirror_far_outside", L"How far mirrors and water show things outdoors. Shorter is faster. 0 uses the game's value. Takes effect next start."},
    {"stereo.mirror_clip", L"Cuts the mirror picture at the glass so nothing behind it shows. Objects can sometimes pop in and out."},
    {"stereo.reflections_off", L"Turns off all mirror and water reflections. A big speed gain in rooms with mirrors. Takes effect next start."},
    // Graphics > Smooth motion / eyes / tests
    {"stereo.turn_reprojection", L"Keeps the picture smooth while you turn with the stick, even when the game is slower than the headset."},
    {"stereo.turn_display_clock", L"Times stick turning by the headset's refresh instead of the game's, so turning stays even."},
    {"stereo.walk_display_clock", L"Times stick walking by the headset's refresh instead of the game's, so walking stays even."},
    {"stereo.turn_lead", L"Draws the picture slightly ahead of where you are turning, which hides lag while turning."},
    {"stereo.paired_eyes", L"While walking, both eyes are drawn from the same moment so the picture doesn't shimmer."},
    {"stereo.paired_hands", L"While walking, your hands are placed the same in both eyes so they don't double."},
    {"stereo.frame_clock", L"The headset gets frames on its own steady clock instead of waiting for the game. Smoother turning."},
    {"stereo.headset_width", L"Stops the game drawing picture the headset never shows. Faster."},
    {"stereo.body_layer", L"Draws your own body separately so it stays steady while you turn."},
    {"stereo.depth_eye", L"Test: draws one eye plus depth and builds the other from it. Faster, but can leave ghost edges."},
    {"stereo.flat_world", L"The world is drawn once for both eyes: clean and fast, but with no depth in the picture. Hands and menus keep their depth."},
    {"stereo.both_eyes", L"Test: draws both eyes fully every frame. Real depth, but needs a strong graphics card."},
    {"stereo.both_eyes_skip", L"Test: the right eye copies one part of the scene from the left eye to save time. Starts Off every start."},
    {"stereo.depth_eye_debug", L"Test views that show how the depth eye is built. For debugging only; leave Off."},
    {"stereo.depth_eye_alternate", L"The two eyes take turns being drawn, so the picture behind near things is real and not guessed."},
    {"stereo.made_eye_shift_x", L"Moves the built eye's picture sideways. Use it to line up the two eyes."},
    {"stereo.made_eye_shift_y", L"Moves the built eye's picture up or down. Use it to line up the two eyes."},
    {"stereo.made_eye_distance", L"How far apart the two eyes' views are. 100% is your real eye distance."},
    {"stereo.made_eye_depth", L"How strong the depth effect of the built eye is. 100% is natural."},
    {"stereo.made_eye_submit_x", L"Tells the headset the built eye sits slightly left or right. Fine alignment."},
    {"stereo.made_eye_submit_yaw", L"Tells the headset the built eye is turned slightly left or right. Fine alignment."},
    {"stereo.made_eye_submit_pitch", L"Tells the headset the built eye is tilted slightly up or down. Fine alignment."},
    {"stereo.probe_double", L"Test: draws the scene twice and logs how long it takes. Slows the game; leave Off."},
    {"stereo.reuse_shadows", L"With both real eyes: the right eye reuses the left eye's shadows. Faster, looks the same."},
    {"stereo.skip_shadow_prep", L"The right eye skips preparing sun shadows again. Faster, looks the same."},
    {"stereo.shadow_map", L"Size of the shadow picture. Bigger is sharper but slower. 0 is the game's own. Takes effect next start."},
    // Performance and tests
    {"perf.sim_cache_boost", L"Gives the game bigger caches for Sims so they load in with fewer hitches. Takes effect next start."},
    {"perf.sim_lod_scale", L"How far away Sims keep full detail. Higher means fewer pop-ins and stalls but a little more work. Takes effect next start."},
    {"perf.hitch_catcher", L"Writes every stall longer than a moment to the log, so slowdowns can be found. Takes effect next start."},
    // Shadows
    {"body.steady_shadows", L"Updates shadows in steps so they don't crawl while you move your head."},
    {"body.shadow_step", L"How much the sun has to move before shadows are redrawn. Smaller is smoother but costs more."},
    {"body.shadow_sharp", L"How far from you shadows stay sharp. Further looks better but costs speed."},
    // Head and body
    {"body.smoothing", L"How quickly the view follows your Sim's head. Higher is tighter, lower is softer."},
    {"body.turn_smoothing", L"How much your view turns along when your Sim turns."},
    {"body.eye_inset", L"Moves the view back from the Sim's eyes so you don't see the inside of the head."},
    {"body.head_x", L"Moves your view sideways from the default eye place, in the head's frame. Debug: find where you don't see into your body."},
    {"body.head_y", L"Moves your view up (+) or down (-) from the default eye place. Debug: find where you don't see into your body."},
    {"body.head_z", L"Moves your view ahead (+) or back (-) from the default eye place. Debug: find where you don't see into your body."},
    {"body.head_tracking", L"Your Sim's head turns to where you look."},
    {"body.seat_follow", L"When seated, your Sim leans as you lean."},
    {"body.seat_torso", L"When seated, moving your head also moves your Sim's upper body."},
    {"body.hide_face", L"Hides your own face so it doesn't block the view. Mirrors still show it."},
    {"body.steady_shoulders", L"Stops the wrist HUD from moving with your Sim's idle shoulder motion."},
    // Near clipping
    {"body.near_clip", L"How close things can get to your eyes before they disappear. Smaller shows more but can show insides of objects."},
    {"body.near_adaptive", L"Near clipping shrinks automatically when something is close to your face."},
    {"body.near_max", L"The largest near clipping the automatic mode may use."},
    {"body.rooms_all", L"Draws every room whole in first person, so things don't vanish after you pass a door. Off if it costs speed."},
    {"body.rooms_whole", L"Keeps rooms you stand next to in a doorway drawn completely, so walls don't blink out."},
    // Controls: moving / turning
    {"body.walk_speed", L"How fast you walk with the left stick."},
    {"body.run_speed", L"How fast you run when you tap the left stick."},
    {"body.turn_rate", L"How fast you turn with the right stick when snap turning is off."},
    {"controls.snap_turn", L"On: the right stick turns in jumps. Off: it turns smoothly. Jumps are easier on the stomach."},
    {"controls.snap_angle", L"How far one gentle push of the stick turns you."},
    {"controls.snap_angle_big", L"How far a full push turns you. 0 uses the gentle angle."},
    {"controls.recenter_turns_body", L"When you recenter the view, your Sim's body turns to face where you look. During actions the view turns to the body instead."},
    {"controls.turn_speed", L"How fast the right stick turns the camera when you are in the game camera view."},
    {"body.carried_stairs", L"Stairs and ladders carry you up and down by themselves."},
    {"body.keep_height", L"Your body is drawn at your real height, also on stairs."},
    {"body.smooth_slide", L"Sliding along walls instead of stopping dead when you walk into them."},
    {"body.collision", L"Your body stops at walls and furniture, also when you walk or lean in your room. Off: the older stick-only check. Needs the map row above on."},
    {"body.body_radius", L"How far your body stays from walls. Smaller gets you closer; larger clips less."},
    {"body.furniture_radius", L"How far your body stays from furniture. Small lets you squeeze between pieces placed close together (Sims keep 12 cm); larger keeps you further off."},
    {"body.collision_scene", L"Keeps a map of the walls and furniture around you for collisions (your body, your hands, touch sounds). Off: none of those, and a little time saved on the game's thread."},
    {"body.hand_object_gap", L"How close your hand comes to furniture. 0 = it touches the shape the game draws; plus keeps it that far off, minus lets it sink in. Move it while touching something to find what looks right."},
    {"body.hand_wall_gap", L"How close your hand comes to walls. 0 = it touches the wall's surface; plus keeps it that far off, minus lets it sink in."},
    {"body.hand_collision", L"Your Sim's hands stop at walls and furniture while your real hands go on, and catch up as you pull back. No vibration. Needs the map row on."},
    {"body.walk_clips", L"Plays the Sim's start and stop walking animations as you move."},
    {"body.crouch", L"Push the right stick down to crouch, again to stand. Only while you walk freely (not in an animation, action, scene or seat)."},
    {"body.crouch_depth", L"How far your view sinks when you crouch."},
    {"body.crouch_speed", L"How fast you walk while crouched, as a share of normal."},
    {"body.crouch_seconds", L"How long going down or standing up takes."},
    {"body.stop_time", L"How long your Sim takes to come to a stop. 0 stops at once."},
    {"body.stop_glide", L"Keeps sliding forward over the stop walking animation's length (about 0.9 m) instead of stopping where you let go."},
    // Hands and touch
    {"body.hand_pitch", L"Tilts the virtual hands up or down against your controllers so they feel natural."},
    {"hands.eat", L"Food goes to your mouth by bringing it there with your hand. Takes effect next start."},
    {"physical.haptics", L"The controller vibrates when your hand touches something."},
    {"hands.grab", L"Lets you pick objects up and hold them with the grip button. Takes effect next start."},
    {"body.god_hands", L"In god mode (the regular game camera) the hands of the Sim you have selected show at your controllers, no arms."},
    {"body.touch", L"Pressing things with your fingertip clicks them. Takes effect next start."},
    {"voice.mic", L"Your microphone moves your Sim's mouth while you talk. Takes effect next start."},
    // Game settings: reach and walking
    {"body.reach_only", L"You can only use things within 1 m of your hands. Walk up to them first."},
    {"body.grey_far", L"Menu choices for things that are too far away are greyed out."},
    {"body.reactions_in_place", L"Your Sim's reactions (laughing, flinching) don't stop you or move you."},
    {"body.no_errands", L"The game can't walk your Sim to things by itself while you play."},
    {"body.route_guard", L"Stops walks your Sim started without you asking."},
    {"body.trail", L"Choosing something far away shows a footprint trail to walk along yourself instead of the Sim walking there."},
    {"body.trail_stop", L"When you walk the trail to a Sim, they put their other actions off, stop and turn to face you once you are close."},
    {"body.trail_stop_dist", L"How near you must be for that Sim to stop and face you."},
    {"body.trail_arrive", L"How close to the end of the trail you must be for the action to start."},
    {"body.trail_arrive_contact", L"The same for socials where you touch someone, usually closer."},
    {"body.load_eyes_closed", L"The game starts with your eyes closed and time paused until you can move, so loading isn't a blur."},
    {"body.exit_eyes_closed", L"Your eyes close while the game saves on exit, so you don't see the freeze."},
    // Other Sims
    {"physical.bodies", L"Other Sims' bodies block your hands, so you can't push through them."},
    {"physical.hold_hands", L"You can hold other Sims' hands."},
    {"physical.look_at_you", L"Other Sims look at you when you are close and talking."},
    {"physical.head", L"Your head is pushed out of other Sims so you can't see through their heads."},
    {"physical.head_radius", L"How far your head keeps away from other Sims."},
    {"physical.grab_bodies", L"Grip and trigger together grab another Sim's body."},
    {"physical.grab_curl", L"How far your fingers close when you grab a body."},
    {"physical.grab_drag", L"How far you can drag a grabbed Sim."},
    {"gestures.wave", L"An open hand up, palm out, swung side to side: the Sim you look at waves back."},
    {"gestures.come_hither", L"Open palm up or toward you, fingers pulled in a few times: the Sim walks over (1.5 to 15 m)."},
    {"gestures.follow_me", L"Hand upright, palm to your face, grip held and trigger pressed twice: the Sim follows you (within 5 m)."},
    {"gestures.follow_reactions", L"Before following they react by mood, traits and relationship: a smile, a flirty look, an eye roll when angry."},
    {"gestures.follow_refuse", L"A Sim who dislikes you (or is in a foul mood with you) refuses to follow, stomping their foot."},
    {"gestures.wave_dismiss", L"Waving at a Sim who follows you: they wave back and stop following (like Dismiss)."},
    {"gestures.nod", L"The Sim nods at you as they start walking to you or following you."},
    {"gestures.haptics", L"The controller buzzes when a Sim answers your wave or come hither."},
    // Touch sounds (foley.py)
    {"foley.on", L"Your hand touching another Sim makes the game's own foley at your hand: soft, a pat or a slap by how fast it hits, cloth or skin by where."},
    {"foley.pat_speed", L"Hitting a Sim slower than this is a light touch; from this on it is a pat."},
    {"foley.slap_speed", L"Hitting a Sim with an open hand at least this fast is a slap (your hand's own speed: walking doesn't count)."},
    {"foley.punch_speed", L"Hitting a Sim with a fist (grip + trigger) at least this fast is a punch."},
    {"foley.walk_hits", L"Off: while the stick moves you, touching a Sim is never a slap or punch, however fast your hand goes."},
    {"knockout.on", L"Hit a Sim hard enough or often enough and it collapses on the floor (the game's Energy Failure), gets up 20 Sim minutes later."},
    {"knockout.score", L"Each slap adds its speed squared to the Sim's daze (a punch 1.5 times that); at this much it is knocked out. 3 m/s slap = 9, 3 m/s punch = 13.5."},
    {"knockout.one_blow", L"A single slap or punch at least this fast knocks a Sim out at once. 0: never in one blow."},
    {"knockout.unconscious", L"On: a knocked-out Sim lies unconscious (Get to Work's collapsed patient: no snoring, no energy change, onlookers react). Off: it falls asleep on the floor (Energy Failure)."},
    {"knockout.minutes", L"How many Sim minutes a knocked-out Sim stays down before it gets up."},
    {"knockout.recover", L"How fast a Sim gets over your blows: its daze halves every this many seconds."},
    {"foley.knockback", L"A slap or punch knocks the part you hit along the blow and it springs back. 0% none, 200% twice as far."},
    {"foley.rub_speed", L"Sliding your hand along a Sim at least this fast makes rub sounds. 0 turns them off."},
    {"foley.own_sounds", L"Your own sound files (sounds folder: touch, pat, slap, punch, grab) play instead of the game's. A fist (grip + trigger) hitting at punch speed or faster is a punch."},
    {"foley.own_volume", L"How loud all your own touch sounds are (100% = as recorded). The game's touch sounds follow the game's Effects volume."},
    {"foley.volume_touch", L"How loud your own light-touch sounds are."},
    {"foley.volume_pat", L"How loud your own pat sounds are."},
    {"foley.volume_slap", L"How loud your own slap sounds are."},
    {"foley.volume_punch", L"How loud your own punch sounds are (a fist: grip + trigger)."},
    {"foley.volume_grab", L"How loud your own grab sounds are (grip + trigger on their body)."},
    {"foley.objects", L"Your hand hitting a table, wall, fridge or sofa makes a sound: a tap, an open-hand slap or a punch (fist), by speed and what it is made of. Your own sounds (sounds\\objects) first."},
    {"foley.object_tap_speed", L"Hitting an object slower than this makes no sound; from this on it is a tap (slap and punch speeds as for Sims)."},
    {"foley.object_tap", L"The game's sound for tapping or knocking on a hard object (when you have none of your own). Picking one plays it."},
    {"foley.object_slap", L"The game's sound for slapping a hard object with an open hand. Picking one plays it."},
    {"foley.object_punch", L"The game's sound for punching a hard object (fist: grip + trigger). Picking one plays it."},
    {"foley.object_metal", L"The game's sound for any hit on metal (fridge, stove, locker). Picking one plays it."},
    {"foley.object_soft", L"The game's sound for any hit on something soft (sofa, bed, cushion). Picking one plays it."},
    {"foley.volume_object_tap", L"How loud your own object tap sounds are."},
    {"foley.volume_object_slap", L"How loud your own object slap sounds are."},
    {"foley.volume_object_punch", L"How loud your own object punch sounds are."},
    {"foley.hands", L"Your hand meeting a Sim's hand at a pat or faster (a high five). Picking one plays it."},
    {"foley.hold_hands", L"Closing your hand around a Sim's hand. Picking one plays it."},
    {"foley.light_cloth", L"A slow touch on clothes. Picking one plays it."},
    {"foley.pat_cloth", L"A pat on clothes. Picking one plays it."},
    {"foley.slap_cloth", L"A slap on clothes. Picking one plays it."},
    {"foley.rub_cloth", L"Sliding along clothes. Picking one plays it."},
    {"foley.grab_cloth", L"Grabbing a clothed part (grip + trigger). Picking one plays it."},
    {"foley.light_skin", L"A slow touch on bare skin (face, hands, bare arms and legs). Picking one plays it."},
    {"foley.pat_skin", L"A pat on bare skin. Picking one plays it."},
    {"foley.slap_skin", L"A slap on bare skin. Picking one plays it."},
    {"foley.rub_skin", L"Sliding along bare skin. Picking one plays it."},
    {"foley.grab_skin", L"Grabbing a bare part (grip + trigger). Picking one plays it."},
    {"physical.grab_reactions", L"Sims react when you grab them: where you grab, how well they know you, their traits and mood decide how."},
    {"physical.touch_reactions", L"Sims react when your hand touches them without grabbing. A fast slap is taken worse than a gentle touch."},
    {"physical.touch_relationship", L"Welcome touches raise friendship (and romance on intimate places); unwelcome ones lower it."},
    {"physical.touch_interrupts", L"Slapped, hit or touched where they don't want it, a Sim drops what it is doing (talking too) and reacts at once. Someone who really likes you keeps chatting through a casual touch."},
    {"physical.touch_consequences", L"After a touch they may slap you back, yell, tell you off, cry, run away, or hug, flirt and kiss you, by how they took it."},
    {"physical.consequence_chance", L"Scales how likely those follow-ups are. 0 never, 1 normal, 3 nearly always."},
    {"physical.touch_fights", L"A hard hit, or the third hit, can make a Sim who dislikes you (or a hot-head) fight you."},
    {"physical.touch_witnesses", L"Sims nearby who see an unwelcome intimate touch call you out."},
    {"physical.show_feelings", L"A Sim close by looking at you shows now and then how it feels about you: angry, an eye roll, a smile, a flirty look. Needs They look at you."},
    {"physical.look_at_hand", L"A Sim you touch or grab looks at your hand for a moment, then back at you."},
    {"physical.look_speed", L"How fast a Sim's look glides to a new place (your hand, your eyes). Higher is quicker."},
    {"physical.look_eyes_within", L"A touch this close to the Sim's eyes is followed with the eyes only, the head stays put."},
    {"physical.look_head_from", L"From this far from the Sim's eyes the whole head turns to the touch; in between, head and eyes share it."},
    {"body.follow_distance", L"Sims you talk with follow you when you walk further than this away. 0 turns following off."},
    {"body.talk_distance", L"A picked social starts at once when the other Sim is within this distance."},
    {"body.socials_in_place", L"Chats and socials with you stay where you are standing; nobody walks you around."},
    // Interface: pie
    {"ui.own_pie", L"On: the mod draws the pie menu itself. Off: it is cut from the game's own screen."},
    {"ui.chat_pie", L"A Sim's question to you is answered on the palm pie, as set by Pie menu shows."},
    {"ui.pie_mode", L"Where the pie menu opens: in front of you, on your other palm, or whichever fits the moment."},
    {"ui.pie_palm_size", L"How big the pie on your palm is."},
    {"ui.pie_palm_lift", L"How far above your palm the pie floats."},
    {"ui.self_pie_poke", L"Poking your own stomach opens your Sim's own pie menu."},
    {"ui.self_pie_palm", L"Pointing the laser at your palm opens your Sim's own pie menu."},
    {"ui.pie_distance", L"How far in front of you the pie menu opens."},
    {"ui.pie_width", L"How big the pie menu is."},
    {"ui.pie_at_hand", L"The pie menu opens at your hand instead of in front of your face."},
    // Pickers
    {"ui.object_menus", L"Choices for fridges, TVs and similar open on the object itself."},
    {"ui.object_menu_width", L"How wide those pickers are on the object."},
    {"claude.tvs", L"Adds the Open Claude option to TVs as well as computers."},
    // Popups
    {"ui.popups_3d", L"Popups appear in the room in front of you instead of on a flat screen."},
    {"ui.others_decide", L"When another Sim has to decide something, the game picks for them instead of asking you."},
    {"ui.popup_distance", L"How far in front of you popups appear."},
    {"ui.keyboard", L"A keyboard opens in front of you when a text box (search, name) has the focus. Point or poke its keys."},
    {"ui.keyboard_size", L"The keyboard's width."},
    {"ui.popup_size", L"How big popups are."},
    {"wrist.no_dimming", L"Removes the blue dimming behind dialogs."},
    // Panels
    {"ui.distance", L"How far in front of you the game's menu panel floats."},
    {"ui.width", L"How big the game's menu panel is."},
    {"ui.height", L"Raises or lowers the menu panel relative to your eyes."},
    {"quad.distance", L"How far away the flat game screen is."},
    {"quad.width", L"How big the flat game screen is."},
    {"ui.wrist_width", L"How wide the game's menu is when it sits on your wrist."},
    // Wrist HUD
    {"wrist.hud", L"Master switch for all wrist HUD pieces. Each piece also has its own switch."},
    {"wrist.scale", L"Sizes every wrist HUD piece together. Each piece also has its own size."},
    {"wrist.watch", L"Shows the time and speed on your left wrist."},
    {"wrist.pause", L"Shows a pause symbol while the game is paused."},
    {"wrist.sleep_clock", L"Shows a clock in front of you while your Sim sleeps."},
    {"wrist.lot_intro", L"When a lot loads, your eyes stay shut and the lot's name, household and traits show in front of you as the game's own arrival banner does; the eyes open when it ends. Pushing the stick skips it."},
    {"wrist.needs", L"Shows your Sim's needs on the inside of your right forearm."},
    {"wrist.moodlets", L"Shows your Sim's moodlets on the inside of your right forearm."},
    {"wrist.actions", L"Shows the queue of actions on your left forearm."},
    {"wrist.build_button", L"A build button on your left wrist. Off: it stays on the phone only."},
    {"wrist.wants", L"Shows your Sim's wants above you."},
    // Notification card
    {"phone.note_place", L"Where notification cards appear in front of you. You can also drag one with the laser grip."},
    {"phone.note_size", L"How wide notification cards are."},
    // Bubbles
    {"ui.bubbles_3d", L"Speech bubbles and skill bars appear as cards in the world next to Sims."},
    {"bubbles.own_in_front", L"Your own bubbles appear in front of you instead of over your Sim's head (first person)."},
    {"bubbles.own_size", L"How big your bubble is."},
    {"bubbles.npc_size", L"How big other Sims' bubbles are."},
    {"bubbles.hide_own", L"Hides your Sim's bubbles."},
    {"bubbles.hide_npc", L"Hides other Sims' bubbles."},
    {"bubbles.cut_others", L"Old way: skill bars, flyaways and nameplates are cut from the game's screen. May leave stray boxes."},
    {"bubbles.own_balloons", L"On: the mod draws balloons and skill bars itself. Off: they are cut from the game's screen."},
    {"bubbles.own_bubble_position", L"Where your bubble floats relative to your view. A green box shows the place while this page is open."},
    {"bubbles.skill_position", L"Where your skill bar floats relative to your view. A green box shows the place while this page is open."},
    {"bubbles.skill_size", L"How wide your skill bar is."},
    {"bubbles.scandal_hide", L"Hide the purple scandal mark from Royalty & Legacy on your Sim."},
    {"bubbles.scandal_position", L"Offset of the purple scandal mark from where the game puts it (your head). A green box shows the offset while this page is open."},
    {"bubbles.hide_pops", L"Hides the small pops over your Sim (the '!' and similar)."},
    {"bubbles.pop_position", L"Where those pops float relative to your view. A green box shows the place while this page is open."},
    {"bubbles.pop_size", L"How big those pops are."},
    // Phone
    {"phone.phone", L"A phone rests at your hip; bring your hand there to take it out."},
    {"phone.notes_overhead", L"Notices about one Sim show over their head. Off: only on the phone."},
    {"phone.hip_drop", L"How far below your eyes your hand must be to take out the phone."},
    {"phone.sound_mode", L"Sound: the tones play and the controllers buzz. Vibrate: only the buzz. Mute: neither."},
    {"phone.ringtone", L"The radio station a phone call rings with: a song of that station, up to 20 seconds or until the call's popup goes."},
    {"phone.text_tone", L"The tone for texts and new notifications (the game's notification sounds)."},
    {"phone.text_size", L"How big the texts on your phone are (the phone's Settings > Phone Options has this too)."},
    {"phone.popup_buzz", L"The phone buzzes when the game shows a popup (first person only)."},
    {"phone.tilt", L"Tilts the phone sideways in your hand."},
    {"phone.yaw", L"Turns the phone flat in your hand."},
    {"phone.pitch", L"Tips the phone back or forward."},
    {"phone.phone_offset", L"Slides the phone against your hand, one direction per row."},
    {"phone.finger_curl", L"How far your fingers curl around the phone."},
    {"phone.thumb_bend", L"How much the thumb bends."},
    {"phone.thumb_spread", L"How far the thumb spreads out."},
    {"phone.hand_offset", L"Moves the hand away from the phone, one direction per row."},
    {"phone.hand_turn", L"Turns the hand on the phone, one axis per row."},
    {"simsforever.phone_finger_curl_left", L"Sims Forever on the phone: how far the left hand's fingers curl around it."},
    {"simsforever.phone_thumb_bend_left", L"Sims Forever on the phone: how much the left thumb bends."},
    {"simsforever.phone_thumb_spread_left", L"Sims Forever on the phone: how far the left thumb spreads out."},
    {"simsforever.phone_hand_offset_left", L"Sims Forever on the phone: where the left hand sits from the phone's middle, one direction per row."},
    {"simsforever.phone_hand_turn_left", L"Sims Forever on the phone: turns the left hand, one axis per row."},
    {"simsforever.phone_finger_curl_right", L"Sims Forever on the phone: how far the right hand's fingers curl around it."},
    {"simsforever.phone_thumb_bend_right", L"Sims Forever on the phone: how much the right thumb bends."},
    {"simsforever.phone_thumb_spread_right", L"Sims Forever on the phone: how far the right thumb spreads out."},
    {"simsforever.phone_hand_offset_right", L"Sims Forever on the phone: where the right hand sits from the phone's middle, one direction per row."},
    {"simsforever.phone_hand_turn_right", L"Sims Forever on the phone: turns the right hand, one axis per row."},
    {"book.book", L"A notebook rests at your left hip; bring your hand there to take it out. It shows what the game's notebook shows."},
    {"book.poke", L"Hold the grip and poke the notebook's pages with your finger, or point the laser at them, to open categories, open entries and turn pages. Without the grip your hand only sweeps to turn pages."},
    {"book.sound", L"Plays a paper page-turn sound when a page turns."},
    {"book.crop_outer", L"Cuts this much off the page picture's outer edge (the baked-in leather rim); the rest is zoomed to fill the page."},
    {"book.crop_spine", L"Cuts this much off the page picture's spine edge (the rings and stack); the rest is zoomed to fill the page."},
    {"book.crop_top", L"Cuts this much off the top of the page picture; the rest is zoomed to fill the page."},
    {"book.crop_bottom", L"Cuts this much off the bottom of the page picture (the baked-in leather); the rest is zoomed to fill the page."},
    {"book.hip_drop", L"How far below your eyes your hand must be to take out the notebook."},
    {"book.tilt", L"Tilts the notebook sideways in your hand."},
    {"book.yaw", L"Turns the notebook flat in your hand."},
    {"book.pitch", L"Tips the notebook back or forward."},
    {"book.book_offset", L"Slides the notebook against your hand, one direction per row."},
    {"book.finger_curl", L"How far your fingers curl around the notebook."},
    {"book.thumb_bend", L"How much the thumb bends."},
    {"book.thumb_spread", L"How far the thumb spreads out."},
    {"book.hand_offset", L"Moves the hand away from the notebook, one direction per row."},
    {"book.hand_turn", L"Turns the hand on the notebook, one axis per row."},
    {"book.book_width", L"Width of the open notebook, both pages together."},
    {"book.open_seconds", L"How long the cover takes to swing open."},
    {"book.swipe", L"How far your free hand must sweep along the notebook to turn a page."},
    {"phone.phone_width", L"Phone width."},
    {"phone.phone_height", L"Phone height."},
    {"phone.phone_depth", L"Phone thickness."},
    {"phone.bezel", L"Width of the border around the phone screen."},
    {"phone.crop_width", L"How much of the width of the game's menu fits on the phone screen."},
    {"phone.crop_height", L"How much of the height of the game's menu fits on the phone screen."},
    {"phone.game_phone", L"Shows the game's own phone model, which also appears in mirrors. Applies next time the phone comes out."},
    {"phone.prop_offset", L"Moves the game's phone model on your phone, one direction per row. Applies next time the phone comes out."},
    {"phone.prop_rotation", L"Turns the game's phone model on your phone, one axis per row. Applies next time the phone comes out."},
    // Tablet
    {"tablet.tablet", L"In first-person build/buy a tablet appears for the build menus."},
    {"tablet.width", L"Tablet width."},
    {"tablet.height", L"Tablet height. 0 fits the layout."},
    {"tablet.depth", L"Tablet thickness."},
    {"tablet.bezel", L"Width of the border around the tablet screen."},
    {"tablet.hold", L"How far in from the edge your hand holds the tablet."},
    {"tablet.menu_size", L"Size of the whole build UI on the tablet, scaled by the mod: each part keeps its place at the screen's edges or middle. 100 % = the UI's height fills the screen. Changes live."},
    {"tablet.tray_scale", L"Size of the tool tray on the tablet's right side. Changes live."},
    // Sound
    {"audio.follow_head", L"Sound comes from where your head is, not from the camera. Takes effect next start."},
    {"audio.voice_near", L"Voices the game plays flat (WickedWhims) are full volume up to this far away."},
    {"audio.voice_far", L"Those voices fade to silence by this distance. 0 keeps the voice's own setting."},
    {"audio.pan_any_distance", L"Far away sounds still come from their direction instead of the middle."},
    {"audio.doors_matter", L"Outdoor sound comes in through open doorways."},
    {"audio.railings_outside", L"Porches with railings count as outside for sound."},
    {"audio.closed_door", L"How much sound gets through a closed door. 0 blocks it, 1 lets it all through."},
    {"passive.on", L"Objects around you affect you without you using them (music playing, a TV on). Takes effect next time you enter first person."},
    {"passive.log", L"Writes what the game's environment score does to you into the python log (lines starting passive:)."},
    {"passive.music", L"A stereo that plays in your room gives a little fun, like a TV on nearby."},
    {"passive.music_range", L"Metres from a playing stereo where its music reaches you. The same room always counts."},
    {"passive.music_rate", L"Fun gained per game minute while music plays near you."},
    {"tv.gaze", L"Looking at a TV that is on watches it, where you stand. Takes effect next time you enter first person."},
    {"tv.nearby", L"Metres from a TV that is on where it gives you a little fun (the same room always counts)."},
    {"tv.passive_share", L"Fun from a TV on nearby, as a share of what really watching it gives."},
    {"audio.wall_leak", L"How much sound gets through walls. 0 blocks it, 1 lets it all through."},
};

// Tips for rows without an ini key (buttons, switches in the game), by label.
const TipText kLabelTips[] = {
    {"First person / game camera", L"Switches between the first-person view and the game's normal camera."},
    {"Testing cheats", L"Turns the game's testing cheats on or off for this game session. Starts Off each time the game starts."},
    {"Cheat pie menus (every pie menu is the cheat menu)", L"Every pie menu opens as the game's cheat menu (like Shift+click). Needs testing cheats; turning this on turns them on too."},
    {"Fill needs", L"Fills all of your Sim's needs at once."},
    {"Motherlode (+50,000 simoleons)", L"Adds 50,000 simoleons to the household funds."},
    {"Move objects on", L"Lets you move any object in live mode."},
    {"CAS full edit mode", L"Unlocks every option in Create-a-Sim."},
    {"Use sims4vr.ini for everything", L"Removes all your menu changes so sims4vr.ini decides again. Press twice to confirm."},
};

// Tips for the rows that open a page, by the page they open.
const TipText kPageTips[] = {
    {"Options", L"All settings, sorted into five groups."},
    {"Game", L"Rules of play: reaching, other Sims, loading and saving."},
    {"Graphics", L"Picture quality, shadows, mirrors and speed."},
    {"Sound", L"How sound is placed and how walls and doors affect it."},
    {"Controls", L"Moving, turning, hands and your head."},
    {"Interface", L"Menus, HUD, bubbles, phone and panels."},
    {"Reach", L"How far you reach and how the game walks you to things."},
    {"Other Sims", L"How other Sims act towards you."},
    {"Other Sims touch", L"Touching, holding hands and grabbing Sims."},
    {"Loading", L"What you see while the game loads and saves."},
    {"View picture", L"Resolution, sharpness and upscaling."},
    {"View mirrors", L"How far mirrors reach and whether reflections draw."},
    {"View near", L"How close things may get to your eyes."},
    {"View shadows", L"Shadow quality and speed."},
    {"View motion", L"Settings that keep the picture smooth while you move."},
    {"View eyes", L"How the two eyes are drawn, with test options."},
    {"View align", L"Fine alignment of the built eye. Only needed with the depth eye."},
    {"View speed", L"Turn tests, Sim caches and a stall logger."},
    {"Moving", L"Walk and run speed, stairs and stopping."},
    {"Turning", L"Snap or smooth turning and turn speeds."},
    {"Hands", L"Hand angle, picking up, touching and eating."},
    {"View head", L"How the view follows your Sim's head and body."},
    {"HUD", L"The pieces on your wrists and around you. Each has its own page."},
    {"HUD watch", L"Switch, position and size of the watch."},
    {"HUD needs", L"Switch, position and size of the needs display."},
    {"HUD moodlets", L"Switch, position and size of the moodlets."},
    {"HUD queue", L"Switch, position and size of the action queue."},
    {"HUD build", L"Switch, position and size of the build button."},
    {"HUD wants", L"Switch, position and size of the wants."},
    {"HUD pause", L"Switch, position and size of the pause symbol."},
    {"HUD sleep", L"Switch, position and size of the clock while asleep."},
    {"HUD lot intro", L"Switch, position and size of the lot name and traits shown while a load keeps your eyes shut."},
    {"HUD note", L"Where notification cards appear and how big they are."},
    {"Menus pie", L"Where and how the pie menu opens."},
    {"Menus pickers", L"Choice menus that open on objects like fridges and TVs."},
    {"Menus popups", L"Where popups appear and how big they are."},
    {"Menus panels", L"The game's menu panel, the flat screen and the wrist menu."},
    {"Bubbles", L"Speech bubbles, skill bars and balloons."},
    {"Bubbles yours", L"Where your own bubble floats."},
    {"Bubbles skill", L"Where your own skill bar floats."},
    {"Bubbles scandal", L"Where the purple scandal mark sits on your Sim."},
    {"Bubbles others", L"Size and visibility of other Sims' bubbles."},
    {"Bubbles pops", L"The small pops over your Sim, like the '!'."},
    {"Phone", L"Where the phone rests and how it looks in your hand."},
    {"Notebook", L"Where the notebook rests, how it sits in your hand and how pages turn."},
    {"Notebook position", L"Where the notebook sits in your hand."},
    {"Notebook hand", L"How the hand holds the notebook."},
    {"Notebook size", L"Notebook size, the cover opening and page turning."},
    {"Phone position", L"Where the phone sits in your hand."},
    {"Phone hand", L"How the hand holds the phone."},
    {"Phone size", L"Phone size and what the screen shows."},
    {"Phone model", L"The game's phone model that mirrors show."},
    {"Tablet", L"The tablet that opens in first-person build/buy."},
    {"Testing", L"Cheats and buttons for testing."},
};

// Generic tip of a HUD element's own rows (keys <piece>_pos / _turn / _around / _size, section wrist).
const wchar_t* hudTip(const std::string& key, const wchar_t* piece, std::deque<std::wstring>& keep) {
    auto ends = [&](const char* end) {
        const size_t n = strlen(end);
        return key.size() > n && key.compare(key.size() - n, n, end) == 0;
    };
    std::wstring text;
    if (ends("_pos"))
        text = std::wstring(L"Where the ") + piece + L" sits, one direction per row. A green box shows the place while this page is open.";
    else if (ends("_turn"))
        text = std::wstring(L"How the ") + piece + L" is turned, one axis per row.";
    else if (ends("_around"))
        text = std::wstring(L"How far round the arm the ") + piece + L" sits.";
    else if (ends("_size"))
        text = std::wstring(L"How big the ") + piece + L" is.";
    else
        return nullptr;
    keep.push_back(text);
    return keep.back().c_str();
}

// Fills every row that has no tip of its own; the last resort is a line by row kind, so a row added later is never blank.
void applyTips(std::vector<Row>& rows) {
    static std::deque<std::wstring> keep;
    for (Row& r : rows) {
        if (r.tip) continue;
        if (r.kind == Kind::Back) {
            r.tip = L"Back to the page above. B does the same.";
            continue;
        }
        if (r.kind == Kind::Link) {
            for (const TipText& t : kPageTips)
                if (r.target && !wcscmp(r.target, std::wstring(t.key, t.key + strlen(t.key)).c_str())) r.tip = t.tip;
            if (!r.tip) r.tip = L"Opens this group of settings.";
            continue;
        }
        if (r.key && r.section) {
            const std::string full = std::string(r.section) + "." + r.key;
            for (const TipText& t : kTips)
                if (full == t.key) r.tip = t.tip;
            if (!r.tip && !strcmp(r.section, "wrist") && r.page > 0 && r.page < pageCount())
                r.tip = hudTip(r.key, g_pages[r.page].name, keep);
        }
        if (!r.tip) {
            for (const TipText& t : kLabelTips) {
                if (!wcscmp(r.label, std::wstring(t.key, t.key + strlen(t.key)).c_str())) r.tip = t.tip;
            }
        }
        if (!r.tip) {
            switch (r.kind) {
                case Kind::Toggle: r.tip = L"Switch On or Off with the stick or by clicking."; break;
                case Kind::Choice: r.tip = L"Pick an option with the arrows."; break;
                case Kind::Number: r.tip = L"Change the value with the arrows or the stick."; break;
                case Kind::Action: r.tip = L"Press to run."; break;
                default: break;
            }
        }
    }
}
