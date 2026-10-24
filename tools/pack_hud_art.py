"""Packs the game's own UI art (re/liveui/art, colour re-cut) that our HUD
draws into one atlas the DLL embeds: art/hud_art.bin and src/hud_art.h (each
piece's rect in it). hud_art.bin: u32 width, u32 height, u32 runs size, then zlib of straight-alpha
BGRA pixels run-length coded: u32 n; n & 0x80000000 = (n & 0x7fffffff)
transparent pixels, else n literal pixels follow.

Pieces are upscaled (Lanczos, in premultiplied space so edges keep no dark
fringe) by SCALE: the game's art is made for ~1x screen pixels, the wrist is
seen up close.

Usage: python3 tools/pack_hud_art.py   (from the repo root)
"""
import os, struct, zlib
import numpy as np
from PIL import Image

SCALE = 3
ART = 're'
# name in hud_art.h, file under re/liveui/art
PIECES = [
    ('kTray', 'liveui/art/TimeControls/201__TimeControlTray__TimeControlTrayContainer@default.png'),
    ('kTimePill', 'liveui/art/TimeControls/195__HudSeasonsWeatherBG_93@controller_out.png'),
    ('kHoverDisc', 'liveui/art/TimeControls/217.png'),
    # The Inventory app = the game's inventory panel (SimInfoInventoryPanel SWF).
    ('kInvCell', 'liveui/art/SimInfoInventoryPanel/111__InventoryCellViewBackground@default.png'),
    ('kSyCellPersonality', 'liveui/art/SimInfoSummaryPanel/4__TimedAspirationCellView__mcHighlight_personality_23@controller_out.png'),
    ('kSyCellReward', 'liveui/art/SimInfoSummaryPanel/13__mcHighlight_reward_154@controller_out__mcLifestyleHighlight_reward_150@controller_out.png'),
    ('kSyCellPhase', 'liveui/art/SimInfoSummaryPanel/19__mcHighlight_phase_180@controller_out.png'),
    ('kSyCellTradition', 'liveui/art/SimInfoSummaryPanel/27__mcHighlight_tradition_181@controller_out.png'),
    ('kSyCellFear', 'liveui/art/SimInfoSummaryPanel/32__mcHighlight_fear_182@controller_out.png'),
    ('kSyArrow', 'liveui/art/SimInfoSummaryPanel/44__HeaderDropdownButton@down__HeaderDropdownButton@out.png'),
    ('kInvCellSelected', 'liveui/art/SimInfoInventoryPanel/113__InventoryCellViewBackground@selected_listed__InventoryCellViewBackground@selected_spoiled.png'),
    ('kInvDrop', 'liveui/art/SimInfoInventoryPanel/92__InventoryDroppableBackground@up__StackDroppableBackground@up.png'),
    ('kInvRegister', 'liveui/art/SimInfoInventoryPanel/65.png'),
    ('kInvRoundButton', 'liveui/art/SimInfoInventoryPanel/78.png'),
    ('kInvGem', 'liveui/art/SimInfoInventoryPanel/81.png'),
    ('kInvLock', 'liveui/art/SimInfoInventoryPanel/129.png'),
    ('kInvFavorite', 'liveui/art/SimInfoInventoryPanel/138.png'),
    ('kInvStack', 'liveui/art/SimInfoInventoryPanel/147.png'),
    ('kInvMsFavorite', 'liveui/art/SimInfoInventoryPanel/39__MultiselectFavoriteButton@disabled.png'),
    ('kInvMsFavoriteOver', 'liveui/art/SimInfoInventoryPanel/44__MultiselectFavoriteButton@controller_over__MultiselectFavoriteButton@over.png'),
    ('kInvMsSell', 'liveui/art/SimInfoInventoryPanel/54__MultiselectSellButton@controller_selected_up__MultiselectSellButton@disabled.png'),
    ('kInvMsSellOver', 'liveui/art/SimInfoInventoryPanel/57__MultiselectSellButton@controller_over__MultiselectSellButton@controller_selected_over.png'),
    # The Weather app = the game's weather and season tooltip (WeatherAndSeasonTooltip, TimeControls SWF).
    ('kWxHeaderSummer', 'liveui/art/TimeControls/3__TooltipHeader_105@summer.png'),
    ('kWxHeaderFall', 'liveui/art/TimeControls/7__TooltipHeader_105@fall.png'),
    ('kWxHeaderWinter', 'liveui/art/TimeControls/11__TooltipHeader_105@winter.png'),
    ('kWxHeaderSpring', 'liveui/art/TimeControls/15__TooltipHeader_105@spring.png'),
    ('kWxSeasonSummer', 'liveui/art/TimeControls/20__SeasonIcon_Tooltip_106@summer.png'),
    ('kWxSeasonFall', 'liveui/art/TimeControls/22__SeasonIcon_Tooltip_106@fall.png'),
    ('kWxSeasonWinter', 'liveui/art/TimeControls/24__SeasonIcon_Tooltip_106@winter.png'),
    ('kWxSeasonSpring', 'liveui/art/TimeControls/26__SeasonIcon_Tooltip_106@spring.png'),
    ('kWxBarBack', 'liveui/art/TimeControls/29.png'),
    ('kWxBarSpring', 'liveui/art/TimeControls/31__SeasonProgressBarSpring.png'),
    ('kWxBarSummer', 'liveui/art/TimeControls/35__SeasonProgressBar.png'),
    ('kWxBarFall', 'liveui/art/TimeControls/39__SeasonProgressBarFall.png'),
    ('kWxBarWinter', 'liveui/art/TimeControls/43__SeasonProgressBarWinter.png'),
    ('kWxBarFrame', 'liveui/art/TimeControls/47.png'),
    ('kEcoBack', 'liveui/art/TimeControls/51.png'),
    ('kEcoNeutralGreen', 'liveui/art/TimeControls/54__EcoProgressBarNeutralGreen.png'),
    ('kEcoNeutralIndustrial', 'liveui/art/TimeControls/58__EcoProgressBarNeutralIndustrial.png'),
    ('kEcoFrame', 'liveui/art/TimeControls/62.png'),
    ('kEcoHandleNeutral', 'liveui/art/TimeControls/67.png'),
    ('kEcoArrow', 'liveui/art/TimeControls/70.png'),
    ('kEcoGreen', 'liveui/art/TimeControls/75__EcoProgressBarGreen.png'),
    ('kEcoHandleGreen', 'liveui/art/TimeControls/79.png'),
    ('kEcoIndustrial', 'liveui/art/TimeControls/84__EcoProgressBarIndustrial.png'),
    ('kEcoHandleIndustrial', 'liveui/art/TimeControls/88.png'),
    ('kMoon0', 'liveui/art/TimeControls/101__PhaseTooltipIcon_80@new_moon.png'),
    ('kMoon1', 'liveui/art/TimeControls/103__PhaseTooltipIcon_80@waxing_crescent.png'),
    ('kMoon2', 'liveui/art/TimeControls/105__PhaseTooltipIcon_80@first_quarter.png'),
    ('kMoon3', 'liveui/art/TimeControls/107__PhaseTooltipIcon_80@waxing_gibbous.png'),
    ('kMoon4', 'liveui/art/TimeControls/109.png'),
    ('kMoon5', 'liveui/art/TimeControls/111__PhaseTooltipIcon_80@waning_gibbous.png'),
    ('kMoon6', 'liveui/art/TimeControls/113__PhaseTooltipIcon_80@third_quarter.png'),
    ('kMoon7', 'liveui/art/TimeControls/115__PhaseTooltipIcon_80@waning_crescent.png'),
    ('kPauseUp', 'liveui/art/TimeControls/263__TimeControlsPauseButton@disabled__TimeControlsPauseButton@up.png'),
    ('kPauseOver', 'liveui/art/TimeControls/266.png'),
    ('kPauseOn', 'liveui/art/TimeControls/269__TimeControlsPauseButton@controller_selected_over__TimeControlsPauseButton@selected_up.png'),
    ('kOneUp', 'liveui/art/TimeControls/233__TimeControlsSpeedOneButton@disabled__TimeControlsSpeedOneButton@up.png'),
    ('kOneOver', 'liveui/art/TimeControls/236.png'),
    ('kOneOn', 'liveui/art/TimeControls/239__TimeControlsSpeedOneButton@controller_selected_over__TimeControlsSpeedOneButton@selected_up.png'),
    ('kTwoUp', 'liveui/art/TimeControls/253__TimeControlsSpeedTwoButton@disabled__TimeControlsSpeedTwoButton@up.png'),
    ('kTwoOver', 'liveui/art/TimeControls/256.png'),
    ('kTwoOn', 'liveui/art/TimeControls/259__TimeControlsSpeedTwoButton@controller_selected_over__TimeControlsSpeedTwoButton@selected_up.png'),
    ('kThreeUp', 'liveui/art/TimeControls/243__TimeControlsSpeedThreeButton@disabled__TimeControlsSpeedThreeButton@up.png'),
    ('kThreeOver', 'liveui/art/TimeControls/246.png'),
    ('kThreeOn', 'liveui/art/TimeControls/249__TimeControlsSpeedThreeButton@controller_selected_over__TimeControlsSpeedThreeButton@selected_up.png'),
    # Moodlets: the game's BuffBGClip tile (Fine: near white) tinted per mood.
    ('kMoodTile', 'liveui/art/SimHub/180__BuffBGClip_19@Fine.png'),
    # The queue's cancel X.
    ('kCancel', 'liveui/art/InteractionQueue/96.png'),
    # Action queue (InteractionQueue SWF, NormalInteractionButton's mcProgress
    # and MixerInteractionButton): the progress ring's dark back, one of its
    # 100 green spokes, its rim; the side action's small cancel X.
    ('kProgressShadow', 'liveui/art/InteractionQueue/45.png'),
    ('kProgressSpoke', 'liveui/art/InteractionQueue/47.png'),
    ('kProgressRim', 'liveui/art/InteractionQueue/50.png'),
    ('kMixerCancel', 'liveui/art/InteractionQueue/41.png'),
    # Wants: the whim cloud.
    ('kWantCloud', 'liveui/art/SimHub/117.png'),
    ('kFearBubble', 'liveui/art/SimHub/88.png'),  # the spiky bubble of a fear (confrontation want)
    # The build/buy button's icon (GameTray), up and over.
    ('kBuildIcon', 'buildui/art/GameTray/93__BuildBuyButton@controller_disabledOver__BuildBuyButton@disabled.png'),
    # Manage Worlds and Gallery (GameTray btnNeighborhood, btnShare) for the phone's Settings apps.
    ('kManageWorldsIcon', 'buildui/art/GameTray/114__BackToNeighborhoodButton@controller_disabledOver__BackToNeighborhoodButton@disabled.png'),
    ('kGalleryIcon', 'buildui/art/GameTray/154__ShareButton@controller_disabledOver__ShareButton@disabled.png'),
    ('kBuildIconOver', 'buildui/art/GameTray/96__BuildBuyButton@controller_down__BuildBuyButton@flair_over.png'),
    # Popups (Dialog): the two-Sim chat (SimSpeechHeader / SimSpeechFooter):
    # the other Sim's portrait ring and blue speech bubble, your portrait ring
    # and the light answer bubble; the dialog's button (out / over) and the
    # plain dialog's panel (DialogHeaderBackground_9@default).
    # Balloons (ObjectBoundElements Balloon.as, mcBackground frames): the bubble over a Sim's head
    # (speech / thought, shout, sentiment) and the category badge's disc.
    ('kBalloonSpeech', 'liveui/art/ObjectBoundElements/139__BalloonBackground_62@speech.png'),
    ('kBalloonThought', 'liveui/art/ObjectBoundElements/141__BalloonBackground_62@infantSentiment__BalloonBackground_62@thought.png'),
    ('kBalloonShout', 'liveui/art/ObjectBoundElements/143__BalloonBackground_62@shout.png'),
    ('kBalloonSentiment', 'liveui/art/ObjectBoundElements/145__BalloonBackground_62@sentiment.png'),
    ('kBalloonDisc', 'liveui/art/ObjectBoundElements/150.png'),
    # The skill bar (SkillBar.as): track pill, green fill, glass outline, the skill icon's glass disc.
    ('kBarTrack', 'liveui/art/ObjectBoundElements/6.png'),
    ('kBarFill', 'liveui/art/ObjectBoundElements/257.png'),
    ('kBarGlass', 'liveui/art/ObjectBoundElements/9.png'),
    ('kBarDisc', 'liveui/art/ObjectBoundElements/235.png'),
    ('kPortraitRing', 'liveui/art/Dialog/64.png'),
    ('kSpeechBubble', 'liveui/art/Dialog/67.png'),
    ('kYourRing', 'liveui/art/Dialog/107.png'),
    ('kAnswerBubble', 'liveui/art/Dialog/109.png'),
    ('kDialogButton', 'liveui/art/Dialog/241__DialogButtonIconInstance@down__DialogButtonIconInstance@out.png'),
    ('kDialogButtonOver', 'liveui/art/Dialog/247__DialogButtonIconInstance@over__DialogButtonInstance@over.png'),
    ('kDialogPanel', 'liveui/art/Dialog/200__DialogHeaderBackground_9@default.png'),
    # The phone (SimInfoTray: PhonePanel and its screens, the game's own phone
    # menu drawn on our phone, hud.cpp drawPhone): the dark band of the app
    # list's carousel, the pill behind the tool buttons, the battery, the
    # silence button (up / over, silenced up / over), an app's hover frame and
    # notification glow, the carousel arrow, the back button, a list row
    # (over / down), the page dots (other / over / this page), the list's
    # slider; the game menu's three dots (GameTray EscapeMenuBtn) for Options.
    ('kPhoneTint', 'liveui/art/SimInfoTray/56.png'),
    ('kPhonePill', 'liveui/art/SimInfoTray/81.png'),
    ('kPhoneBattery', 'liveui/art/SimInfoTray/78.png'),
    ('kPhoneSilence', 'liveui/art/SimInfoTray/63__PhoneSilenceButton@controller_disabledOver__PhoneSilenceButton@disabled.png'),
    ('kPhoneSilenceOver', 'liveui/art/SimInfoTray/66__PhoneSilenceButton@over.png'),
    ('kPhoneSilenced', 'liveui/art/SimInfoTray/69__PhoneSilenceButton@selected_disabled__PhoneSilenceButton@selected_up.png'),
    ('kPhoneSilencedOver', 'liveui/art/SimInfoTray/72__PhoneSilenceButton@controller_selected_over__PhoneSilenceButton@selected_over.png'),
    ('kPhoneAppOver', 'liveui/art/SimInfoTray/59__PhoneCategoryCellView@over__PhoneCategoryCellView@selected_up.png'),
    ('kPhoneBadgeGlow', 'liveui/art/SimInfoTray/7__MotivesVampireSunlightOverlay@intro__PhoneCategoryCellView@up.png'),
    ('kPhoneArrow', 'liveui/art/SimInfoTray/20__PhoneArrowButton@out__PhoneArrowButton@release.png'),
    ('kPhoneArrowOver', 'liveui/art/SimInfoTray/23__PhoneArrowButton@controller_over__PhoneArrowButton@over.png'),
    ('kAppSettings', '../art/phone_icons/settings.png', 2),  # our phone apps (tools/make_phone_icons.py)
    ('kAppCalendar', '../art/phone_icons/calendar.png', 2),  # our phone apps (tools/make_phone_icons.py)
    ('kAppBuild', '../art/phone_icons/build.png', 2),  # our phone apps (tools/make_phone_icons.py)
    ('kAppGameOptions', '../art/phone_icons/game_options.png', 2),  # our phone apps (tools/make_phone_icons.py)
    ('kAppPhoneOptions', '../art/phone_icons/phone_options.png', 2),  # our phone apps (tools/make_phone_icons.py)
    ('kAppVrOptions', '../art/phone_icons/vr_options.png', 2),  # our phone apps (tools/make_phone_icons.py)
    ('kAppMySim', '../art/phone_icons/my_sim.png', 2),
    ('kAppGodView', '../art/phone_icons/god_view.png', 2),  # Settings > God View
    ('kAppSimsForever', '../art/phone_icons/sims_forever.png', 2),  # Home > Sims Forever (only with Sims 1 set up)
    ('kNeedHunger', '../art/phone_icons/need_hunger.png', 2),  # the Needs app (make_phone_icons.py)
    ('kNeedHygiene', '../art/phone_icons/need_hygiene.png', 2),  # the Needs app (make_phone_icons.py)
    ('kNeedBladder', '../art/phone_icons/need_bladder.png', 2),  # the Needs app (make_phone_icons.py)
    ('kNeedEnergy', '../art/phone_icons/need_energy.png', 2),  # the Needs app (make_phone_icons.py)
    ('kNeedFun', '../art/phone_icons/need_fun.png', 2),  # the Needs app (make_phone_icons.py)
    ('kNeedSocial', '../art/phone_icons/need_social.png', 2),  # the Needs app (make_phone_icons.py)
    ('kAppAspirations', '../art/phone_icons/aspirations.png', 2),
    ('kAppCareer', '../art/phone_icons/career.png', 2),
    ('kAppSkills', '../art/phone_icons/skills.png', 2),
    ('kAppRelationships', '../art/phone_icons/relationships.png', 2),
    ('kAppSimology', '../art/phone_icons/simology.png', 2),
    ('kAppInventory', '../art/phone_icons/inventory.png', 2),
    ('kAppNeeds', '../art/phone_icons/needs.png', 2),
    ('kAppSocialGroups', '../art/phone_icons/social_groups.png', 2),
    ('kAppBusinesses', '../art/phone_icons/owned_businesses.png', 2),
    ('kAppHousehold', '../art/phone_icons/household.png', 2),
    ('kAppWeather', '../art/phone_icons/weather.png', 2),
    ('kAppWants', '../art/phone_icons/wants.png', 2),
    ('kTrayBigger', '../art/tray_icons/bigger.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayCatCheats', '../art/tray_icons/cat_cheats.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayCatObjects', '../art/tray_icons/cat_objects.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayCatRoofs', '../art/tray_icons/cat_roofs.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayCatTerrain', '../art/tray_icons/cat_terrain.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayCatWalls', '../art/tray_icons/cat_walls.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayCopies', '../art/tray_icons/copies.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayEraser', '../art/tray_icons/eraser.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayExitFirstPerson', '../art/tray_icons/exit_first_person.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayFillRoom', '../art/tray_icons/fill_room.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayFreePlace', '../art/tray_icons/free_place.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayGrid', '../art/tray_icons/grid.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayHidden', '../art/tray_icons/hidden.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayLiveEdit', '../art/tray_icons/live_edit.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayLower', '../art/tray_icons/lower.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayMoveObjects', '../art/tray_icons/move_objects.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayOneWall', '../art/tray_icons/one_wall.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayQuarter', '../art/tray_icons/quarter.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayRaise', '../art/tray_icons/raise.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayRoofCurve', '../art/tray_icons/roof_curve.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayRoofEaves', '../art/tray_icons/roof_eaves.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayRoofHeight', '../art/tray_icons/roof_height.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTraySlot', '../art/tray_icons/slot.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTraySmaller', '../art/tray_icons/smaller.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTraySnap', '../art/tray_icons/snap.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTraySwatch', '../art/tray_icons/swatch.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayTurnLeft', '../art/tray_icons/turn_left.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kTrayTurnRight', '../art/tray_icons/turn_right.png', 1),  # the build tablet's tool tray (tools/make_tray_icons.py)
    ('kSpin0', 'liveui/art/ExchangeDetail/152.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin1', 'liveui/art/ExchangeDetail/154.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin2', 'liveui/art/ExchangeDetail/156.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin3', 'liveui/art/ExchangeDetail/158.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin4', 'liveui/art/ExchangeDetail/160.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin5', 'liveui/art/ExchangeDetail/162.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin6', 'liveui/art/ExchangeDetail/164.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin7', 'liveui/art/ExchangeDetail/166.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin8', 'liveui/art/ExchangeDetail/168.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kSpin9', 'liveui/art/ExchangeDetail/170.png'),  # the game's spinning plumbob (DetailLoadingAnimation)
    ('kPhoneBack', 'liveui/art/SimInfoTray/33__PhoneBackButton@out__PhoneBackButton@release.png'),
    ('kPhoneBackOver', 'liveui/art/SimInfoTray/36__PhoneBackButton@over__PhoneBackButton@selected_disabled.png'),
    ('kPhoneRowOver', 'liveui/art/SimInfoTray/11__PhoneItemCellView@over.png'),
    ('kPhoneRowDown', 'liveui/art/SimInfoTray/14__PhoneItemCellView@down.png'),
    ('kPhonePage', 'liveui/art/SimInfoTray/85__PhoneMenu_Button_OtherPage_124@out__PhoneMenu_Button_OtherPage_124@release.png'),
    ('kPhonePageOver', 'liveui/art/SimInfoTray/88__PhoneMenu_Button_OtherPage_124@over__PhonePageButton@over.png'),
    ('kPhonePageOn', 'liveui/art/SimInfoTray/100__PhonePageButton@selected_disable__PhonePageButton@selected_up.png'),
    ('kPhoneSlider', 'liveui/art/SimInfoTray/43__PhoneMenuScrollList.png'),
    ('kPhoneColorBtn', 'liveui/art/SimInfoTray/113__PhoneBackgroundColorSelectorButton@controller_disabledOver__PhoneBackgroundColorSelectorButton@controller_selected_up.png'),
    ('kPhoneColorBtnOver', 'liveui/art/SimInfoTray/116__PhoneBackgroundColorSelectorButton@controller_selected_over__PhoneBackgroundColorSelectorButton@over.png'),
    ('kPhoneColorBtnDown', 'liveui/art/SimInfoTray/119__PhoneBackgroundColorSelectorButton@controller_selected_down__PhoneBackgroundColorSelectorButton@down.png'),
    ('kPhoneSelectorArrow', 'liveui/art/SimInfoTray/47__PhoneBackgroundColorSelector__PhoneCaseSelector.png'),
    ('kNoteDelete', 'liveui/art/NotificationWall/8__DeleteButton@disabled__DeleteButton@up.png'),
    ('kNoteDeleteOver', 'liveui/art/NotificationWall/10__DeleteButton@over.png'),
    # The notification backgrounds (Notifications SWF: QueuedEffectBackground_4's
    # frames standard/player/critical/moment, each with its "_strip" frame).
    ('kNoteStandard', 'liveui/art/Notifications/3__QueuedEffectBackground_4@standard.png'),
    ('kNotePlayer', 'liveui/art/Notifications/7__QueuedEffectBackground_4@player.png'),
    ('kNoteCritical', 'liveui/art/Notifications/11__QueuedEffectBackground_4@critical.png'),
    ('kNoteStandardStrip', 'liveui/art/Notifications/5__QueuedEffectBackground_4@standard_strip.png'),
    ('kNotePlayerStrip', 'liveui/art/Notifications/9__QueuedEffectBackground_4@player_strip.png'),
    ('kNoteCriticalStrip', 'liveui/art/Notifications/13__QueuedEffectBackground_4@critical_strip.png'),
    ('kNoteMomentStrip', 'liveui/art/Notifications/17__QueuedEffectBackground_4@moment_strip.png'),
    # The calendar app (Calendar SWF: CalendarDayView, SchedulePane; the HUD's
    # calendar button), drawn small on the phone: 1x pieces (third field).
    ('kCalendarButton', 'liveui/art/SimInfoTray/404__CalendarButton@over.png'),
    # My Sim's apps (user 2026-10-24 03:37Z: the game's own assets): the SimInfo tray's panel buttons.
    ('kTrayAspiration', 'liveui/art/SimInfoTray/283__AspirationPanelButton@over.png'),
    ('kTrayCareer', 'liveui/art/SimInfoTray/252__CareerPanelButton@over__CareerPanelButton@selected_up.png'),
    ('kTraySkill', 'liveui/art/SimInfoTray/266__SkillPanelButton@controller_selected_down__SkillPanelButton@controller_selected_over.png'),
    ('kTraySummary', 'liveui/art/SimInfoTray/233__SummaryPanelButton@controller_selected_over__SummaryPanelButton@flair_over.png'),
    ('kTrayInventory', 'liveui/art/SimInfoTray/240__InventoryPanelButton@controller_selected_over__InventoryPanelButton@over.png'),
    ('kTrayClub', 'liveui/art/SimInfoTray/184__ClubPanelButton@controller_selected_over__ClubPanelButton@flair_over.png'),
    ('kTrayHousehold', 'liveui/art/SimInfoTray/199__HouseholdInfoVenuePanelButton@controller_selected_over__HouseholdInfoVenuePanelButton@flair_over.png'),
    ('kTrayRelationship', 'liveui/art/SimInfoTray/259__RelationshipPanelButton@flair_over__RelationshipPanelButton@over.png'),
    ('kCalSpring', 'liveui/art/Calendar/207__SeasonalBackgrounds_20@none.png', 1),
    ('kCalSummer', 'liveui/art/Calendar/209__SeasonalBackgrounds_20@summer.png', 1),
    ('kCalFall', 'liveui/art/Calendar/211__SeasonalBackgrounds_20@fall.png', 1),
    ('kCalWinter', 'liveui/art/Calendar/213__SeasonalBackgrounds_20@winter.png', 1),
    ('kCalDayPast', 'liveui/art/Calendar/270__CalendarDayView@past_up.png', 1),
    ('kCalDaySelected', 'liveui/art/Calendar/267__CalendarDayView@over.png', 1),
    ('kCalBgWedding', 'liveui/art/Calendar/26.png', 1),
    ('kCalBgHoliday', 'liveui/art/Calendar/111.png', 1),
    ('kCalBgGetaway', 'liveui/art/Calendar/128.png', 1),
    ('kCalBgFuneral', 'liveui/art/Calendar/142.png', 1),
    ('kCalBgFestival', 'liveui/art/Calendar/155.png', 1),
    ('kCalBgEvent', 'liveui/art/Calendar/170.png', 1),
    ('kCalBgWork', 'liveui/art/Calendar/184__ScheduleWorkEventEntry.png', 1),
    ('kCalBgBirthday', 'liveui/art/Calendar/254.png', 1),
    ('kCalIconWedding', 'liveui/art/Calendar/28__DayViewActivityIcon_30@wedding.png', 1),
    ('kCalIconVacation', 'liveui/art/Calendar/63__DayViewActivityIcon_30@vacation.png', 1),
    ('kCalIconStayover', 'liveui/art/Calendar/76.png', 1),
    ('kCalIconHoliday', 'liveui/art/Calendar/113__CalendarHolidayTooltipEntry__DayViewActivityIcon_30@holiday.png', 1),
    ('kCalIconGetaway', 'liveui/art/Calendar/126__DayViewActivityIcon_30@getaway.png', 1),
    ('kCalIconFuneral', 'liveui/art/Calendar/144__DayViewActivityIcon_30@funeral.png', 1),
    ('kCalIconFestival', 'liveui/art/Calendar/157__CalendarFestivalTooltipEntry__DayViewActivityIcon_30@festival.png', 1),
    ('kCalIconEvent', 'liveui/art/Calendar/172__DayViewActivityIcon_30@event.png', 1),
    ('kCalIconWork', 'liveui/art/Calendar/188__DayViewActivityIcon_30@work.png', 1),
    ('kCalIconBirthday', 'liveui/art/Calendar/200__DayViewActivityIcon_30@birthday.png', 1),
    ('kCalEntryBg', 'liveui/art/Calendar/1.png', 1),
    ('kCalMoon0', 'liveui/art/Calendar/226__CalendarLunarPhaseIcon_29@new_moon.png', 1),
    ('kCalMoon1', 'liveui/art/Calendar/228__CalendarLunarPhaseIcon_29@waxing_crescent.png', 1),
    ('kCalMoon2', 'liveui/art/Calendar/230__CalendarLunarPhaseIcon_29@first_quarter.png', 1),
    ('kCalMoon3', 'liveui/art/Calendar/232__CalendarLunarPhaseIcon_29@waxing_gibbous.png', 1),
    ('kCalMoon4', 'liveui/art/Calendar/234__CalendarLunarPhaseIcon_29@full_moon.png', 1),
    ('kCalMoon5', 'liveui/art/Calendar/236__CalendarLunarPhaseIcon_29@waning_gibbous.png', 1),
    ('kCalMoon6', 'liveui/art/Calendar/238__CalendarLunarPhaseIcon_29@third_quarter.png', 1),
    ('kCalMoon7', 'liveui/art/Calendar/240__CalendarLunarPhaseIcon_29@waning_crescent.png', 1),
    ('kGameMenuDots', 'buildui/art/GameTray/235__EscapeMenuBtn@disabled__EscapeMenuBtn@selected_disabled.png'),
    ('kGameMenuDotsOver', 'buildui/art/GameTray/238.png'),
    # The pie menu (PieMenu SWF): PieMenuButton / BackButton backgrounds (PickerButtonBackground 9-slice,
    # its over and down frames) and the back button's arrow cap.
    ('kPieButton', 'liveui/art/PieMenu/1__BackButton@disabled__PickerButtonBackground_6.png'),
    ('kPieButtonOver', 'liveui/art/PieMenu/41__BackButton@over__PickerButtonBackgroundHighlighted_9.png'),
    ('kPieButtonDown', 'liveui/art/PieMenu/46__BackButton@down__PickerButtonBackgroundDown_11.png'),
    ('kPieBackArrow', 'liveui/art/PieMenu/75.png'),
    # The notebook in your hand (Notebook SWF): the left page with its leather rim and rings (mirrored for the right
    # one), a category cell (up / over), the page-curl corner and the Sim's white portrait ring.
    ('kNbPage', 'liveui/art/Notebook/139.png', 1),
    ('kNbCell', 'liveui/art/Notebook/102__CategoryCellView@down__CategoryCellView@up.png', 2),
    ('kNbCellOver', 'liveui/art/Notebook/107__CategoryCellView@over.png', 2),
    ('kNbCurl', 'liveui/art/Notebook/130__NotebookPage.png', 1),
    ('kNbPortrait', 'liveui/art/Notebook/126.png', 2),
]
EXTRA = 'tools/hud_art_extra.txt'
# Arrays of one piece per mood (MOODS order): the frames of a mood sprite,
# '<dir>/*__<sprite>@<Mood>.png'; Default is the sprite's Fine frame (its
# "Default" label shows the Fine picture). The queue button's disc
# (InteractionButtonEmotionBg_6) and the side action's
# (MixerInteractionButtonEmotionBg_21): InteractionButton.as shows the
# Sim's mood frame when the interaction lists that mood, else Default.
MOOD_ARRAYS = [
    ('kQueueDisc', 'liveui/art/InteractionQueue', 'InteractionButtonEmotionBg_6'),
    ('kMixerDisc', 'liveui/art/InteractionQueue', 'MixerInteractionButtonEmotionBg_21'),
    ('kPieMoodCap', 'liveui/art/PieMenu', 'MoodCap_24'),  # PieMenuButton's mood cap (left end of the pill)
]
# The moods, in the order the script sends them (hudinfo.py MOODS); their
# colour = the game's BuffBGClip_19@<mood> tile over the Fine tile.
MOODS = ['Default', 'Angry', 'Asleep', 'Bored', 'Confident', 'Dazed', 'Embarrassed', 'Energized', 'Fine', 'Flirty',
         'Focused', 'Happy', 'Inspired', 'Playful', 'Sad', 'Scared', 'Tense', 'Uncomfortable']

def mood_tints():
    import glob
    def mean(path):
        a = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32)
        m = a[:, :, 3] > 200
        return a[m][:, :3].mean(0)
    base = mean(os.path.join(ART, 'liveui/art/SimHub/180__BuffBGClip_19@Fine.png'))
    out = []
    for mood in MOODS:
        f = glob.glob(os.path.join(ART, 'liveui/art/SimHub/*__BuffBGClip_19@%s.png' % mood))
        if not f and mood == 'Scared':  # its tile has no label in the SWF; the next BuffBGClip frame (maroon)
            f = [os.path.join(ART, 'liveui/art/SimHub/204.png')]
        c = mean(f[0]) / np.maximum(base, 1) if f else np.ones(3)
        r, g, b = (np.clip(c, 0, 1) * 255 + 0.5).astype(int)
        out.append(0xff000000 | (b << 16) | (g << 8) | r)  # ImGui IM_COL32 order (ABGR)
    return out  # more "name path" lines (needs, moodlets...)

def load(path, scale=SCALE):
    a = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32) / 255
    a[:, :, :3] *= a[:, :, 3:4]  # premultiply
    h, w = a.shape[:2]
    chans = [Image.fromarray(a[:, :, c], 'F').resize((w * scale, h * scale), Image.LANCZOS) for c in range(4)]
    out = np.stack([np.asarray(c) for c in chans], 2).clip(0, 1)
    out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return out

def main():
    pieces = list(PIECES)
    if os.path.exists(EXTRA):
        for line in open(EXTRA):
            line = line.split('#')[0].strip()
            if line:
                n, p = line.split(None, 1)
                pieces.append((n, p))
    import glob
    arrays = []  # (array name, [piece name per mood])
    for name, folder, sprite in MOOD_ARRAYS:
        names = []
        for mood in MOODS:
            frame = 'Fine' if mood == 'Default' else mood
            f = glob.glob(os.path.join(ART, folder, '*__%s@%s.png' % (sprite, frame)))
            if len(f) != 1:
                raise SystemExit('%s: %d pictures for %s' % (name, len(f), frame))
            piece = '%s_%s' % (name, frame)
            if piece not in [t[0] for t in pieces]:
                pieces.append((piece, os.path.relpath(f[0], ART)))
            names.append(piece)
        arrays.append((name, names))
    imgs = [(t[0], load(os.path.join(ART, t[1]), *t[2:])) for t in pieces]
    W = 1024
    x = y = 0; row = 0; rects = []
    for n, im in sorted(imgs, key=lambda t: -t[1].shape[0]):
        h, w = im.shape[:2]
        if x + w + 2 > W: x = 0; y += row + 2; row = 0
        rects.append((n, x, y, w, h, im)); x += w + 2; row = max(row, h)
    H = y + row
    H = (H + 15) // 16 * 16  # (D3D11 textures need no power of two)
    atlas = np.zeros((H, W, 4), np.float32)
    for n, x, y, w, h, im in rects:
        atlas[y:y + h, x:x + w] = im
    # Straight alpha in the file (Dear ImGui blends straight alpha).
    a = atlas[:, :, 3:4]
    atlas[:, :, :3] = np.where(a > 1e-4, atlas[:, :, :3] / np.maximum(a, 1e-4), 0)
    bgra = (atlas[:, :, [2, 1, 0, 3]].clip(0, 1) * 255 + 0.5).astype(np.uint8)
    os.makedirs('art', exist_ok=True)
    # The runs (transparent ones as a count), zlib-packed to keep the DLL small.
    runs = bytearray()
    px = bgra.reshape(-1, 4)
    empty = px[:, 3] == 0
    i, n = 0, len(px)
    while i < n:
        j = i
        if empty[i]:
            while j < n and empty[j]: j += 1
            runs += struct.pack('<I', 0x80000000 | (j - i))
        else:
            while j < n and not empty[j]: j += 1
            runs += struct.pack('<I', j - i); runs += px[i:j].tobytes()
        i = j
    with open('art/hud_art.bin', 'wb') as f:
        f.write(struct.pack('<III', W, H, len(runs)))
        f.write(zlib.compress(bytes(runs), 9))
    with open('src/hud_art.h', 'w') as f:
        f.write('#pragma once\n// Generated by tools/pack_hud_art.py from the game\'s own UI art (re/liveui/art).\n')
        f.write('// Pieces of art/hud_art.bin (%dx%d, straight-alpha BGRA, art upscaled %dx): x, y, w, h in pixels.\n' % (W, H, SCALE))
        f.write('namespace s4vr::hud::art {\nconstexpr int kAtlasW = %d, kAtlasH = %d, kScale = %d;\n' % (W, H, SCALE))
        f.write('struct Piece {\n    int x, y, w, h;\n};\n')
        in_arrays = {n for _, names in arrays for n in names}
        at = {n: (x, y, w, h) for n, x, y, w, h, im in rects}
        for n, x, y, w, h, im in sorted(rects, key=lambda r: [p[0] for p in pieces].index(r[0])):
            if n not in in_arrays:
                f.write('constexpr Piece %s{%d, %d, %d, %d};\n' % (n, x, y, w, h))
        for name, names in arrays:
            f.write('// One per mood, in hudinfo.py MOODS order.\n')
            f.write('constexpr Piece %s[%d] = {%s};\n' % (name, len(names), ', '.join('{%d, %d, %d, %d}' % at[n] for n in names)))
        f.write('// Mood colours (IM_COL32), in hudinfo.py MOODS order: %s.\n' % ', '.join(MOODS))
        f.write('constexpr unsigned kMoodTint[%d] = {%s};\n' % (len(MOODS), ', '.join('0x%08xu' % t for t in mood_tints())))
        f.write('}  // namespace s4vr::hud::art\n')
    print('atlas %dx%d, %d pieces' % (W, H, len(rects)))

if __name__ == '__main__':
    main()
