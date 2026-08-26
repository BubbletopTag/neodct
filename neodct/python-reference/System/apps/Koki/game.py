# Koki game logic, ported 1:1 from the decompiled Scratch project
# (see tools/ and docs/KOKI_PORT_NOTES.md). Each handler mirrors one
# original script; comments name the original hat block.
#
# Conventions:
#   - eng.on(<message>, <sprite>) generators == "when I receive" scripts.
#   - "wait N" -> yield from W(N); "glide" -> yield from spr.glide(...).
#   - "repeat until x == EDGE: change x by S" is ported as pass-the-target
#     (Scratch relied on stage-edge clamping).
#   - Scratch costume-name compares are case-insensitive; so is costume_is().

def register_all(eng):
    W = eng.wait
    V = eng.vars
    key = eng.key
    R = eng.randint

    # ---- playability tuning (sanctioned deviations from the 1:1 port) ----
    # ATK > 1 slows attacks down; IFRAMES = post-hit invincibility seconds.
    import os as _os
    try:
        ATK = float(_os.environ.get("NEODCT_KOKI_ATTACK_SLOW", "1.35"))
    except ValueError:
        ATK = 1.35
    if not (ATK > 0):   # rejects 0 (div-by-zero at 20/ATK), negatives, NaN
        ATK = 1.35
    IFRAMES = 0.9

    # ---- global variables ("when flag clicked" defaults) -------------------
    V.update({
        "lives": 3, "doors": 1, "taken damage": 0, "knockouts": 0,
        "has healed": 0, "cannondefeats": 1, "RibyDanger": 0,
        "evilcanonballdirection": -90, "healwavedirection": 2,
        "damageway4": 2,
    })

    # ---- sprites ------------------------------------------------------------
    PLAYER = eng.sprite("Player")
    ANIM = eng.sprite("CharacterAnim")
    PLAT = eng.sprite("Platform")
    LOGO = eng.sprite("Dynaris Logo")
    INTRO = eng.sprite("intro")
    STARTBTN = eng.sprite("StartButton")
    PANEL = eng.sprite("Sprite1")
    WHITE = eng.sprite("White")
    DOOR1 = eng.sprite("Door1")
    DOOR2 = eng.sprite("Door2")
    DOOR3 = eng.sprite("Door3")
    DOOR4 = eng.sprite("Door4")
    EN1 = eng.sprite("Enemy 1")
    KSTAT = eng.sprite("KokiStats")
    GOVER = eng.sprite("GameOver")
    SW1 = eng.sprite("Shockwave")
    SW2 = eng.sprite("Shockwave2")
    E1STAT = eng.sprite("Enemy1Stats")
    CANNON = eng.sprite("Cannon")
    CBALL = eng.sprite("Cannon ball")
    QUICK = eng.sprite("QuickPress")
    KPSTAT = eng.sprite("KokiPlaneStats")
    CUT1 = eng.sprite("cutscene1")
    PLANE = eng.sprite("PlaneChar")
    EN2 = eng.sprite("Enemy2")
    E2STAT = eng.sprite("Enemy2Stats")
    CANNON2 = eng.sprite("Cannon2")
    GAS = eng.sprite("Gas tank")
    DODGE = eng.sprite("A to Dodge")
    SCORE = eng.sprite("Sprite2")
    ABYSS = eng.sprite("Abyss")
    EN3 = eng.sprite("Enemy 3")
    LIVES = eng.sprite("Lives")
    E3STAT = eng.sprite("Enemy3Stats")
    CANNON3 = eng.sprite("Cannon3")
    CBALL2 = eng.sprite("Cannon ball2")
    E4STAT = eng.sprite("Enemy4Stats")
    EVILC = eng.sprite("EvilCannon")
    CBALL3 = eng.sprite("Cannon ball3")
    SW3 = eng.sprite("Shockwave3")
    SW4 = eng.sprite("Shockwave4")
    SW5 = eng.sprite("Shockwave5")
    CBALL4 = eng.sprite("Cannon ball4")
    REWARD = eng.sprite("Reward")
    RIBY = eng.sprite("Riby")

    # paint order from the original project (back -> front)
    eng.set_layer_order([
        "Door4", "Platform", "Door1", "Door2", "PlaneChar", "Door3",
        "cutscene1", "Cannon ball", "Cannon ball2", "Abyss", "intro",
        "StartButton", "Enemy1Stats", "KokiPlaneStats", "KokiStats",
        "Sprite1", "Shockwave2", "Shockwave5", "Shockwave4", "Shockwave3",
        "Shockwave", "Cannon", "QuickPress", "Enemy2", "Cannon2", "Gas tank",
        "Lives", "Enemy 3", "Player", "Cannon3", "Enemy3Stats",
        "CharacterAnim", "GameOver", "Enemy 1", "Riby", "A to Dodge",
        "Enemy2Stats", "EvilCannon", "Cannon ball4", "Cannon ball3",
        "Reward", "Sprite2", "Dynaris Logo", "White",
    ])

    # initial poses come from the manifest (sb3 editor-left defaults)

    # =========================================================================
    # Stage: backdrops + music
    # =========================================================================
    @eng.on("flag", None)
    def _stage_flag():
        eng.backdrop("backdrop1")
        return
        yield

    @eng.on("start intro", None)
    def _stage_intro():
        eng.stage_music("Koki prototype theme")
        return
        yield

    @eng.on("go to lobby", None)
    def _stage_lobby():
        eng.backdrop("backdrop5")
        eng.stage_music("Koki New Level Lobby")
        return
        yield

    @eng.on("startlv1", None)
    def _stage_lv1():
        eng.backdrop("backdrop3")
        eng.stage_music("Koki Level 1")
        return
        yield

    @eng.on("startlv2", None)
    def _stage_lv2():
        eng.backdrop("backdrop4")
        eng.stage_music("Koki Level 2")
        return
        yield

    @eng.on("startlv3", None)
    def _stage_lv3():
        eng.backdrop("backdrop6")
        eng.stage_music("Popi vs Koki")
        return
        yield

    @eng.on("startfinallevel", None)
    def _stage_final():
        eng.backdrop("backdrop5")
        eng.stage_music("Riby boss fight prototype music 1")
        return
        yield

    @eng.on("ending cutscene", None)
    def _stage_ending():
        eng.backdrop("backdrop2")
        eng.stage_music("lobbykoki")
        return
        yield

    @eng.on("boxing bell", None)
    def _stage_bell():
        eng.stage_sfx("boxing bell")
        return
        yield

    # music stops (original: "stop other scripts in sprite" on the Stage)
    for _msg in ("oofie", "enemy1 oof", "planeoofie", "enemy2 oof",
                 "game over", "falloofie", "enemy 4 oof", "the end",
                 "stopmusic"):
        @eng.on(_msg, None)
        def _stage_stop():
            eng.stop_music()
            return
            yield

    @eng.on("final cutscene", None)
    def _stage_finalcut():
        # original slides music pitch down 400% over ~80 frames; mpv can't,
        # so fade out by just stopping after a beat.
        yield from W(1.0)
        eng.stop_music()

    # =========================================================================
    # Dynaris Logo -> intro -> start button -> info panel
    # =========================================================================
    @eng.on("flag", LOGO)
    def _logo():
        LOGO.set_costume("costume1")
        LOGO.show()
        LOGO.front()
        LOGO.ghost = 100
        for _ in range(20):
            LOGO.ghost -= 5
            yield from W(0.01)
        LOGO.play("Collect Sound Effect")
        yield from W(0.1)
        for _ in range(12):
            LOGO.next_costume()
            yield
        yield from W(0.5)
        for _ in range(20):
            LOGO.ghost += 5
            yield from W(0.01)
        yield from W(1)
        LOGO.ghost = 100
        LOGO.hide()
        eng.broadcast("start intro")

    @eng.on("start intro", INTRO)
    def _intro():
        INTRO.goto(0, 0)
        INTRO.set_costume("Koki Icon")
        eng.backdrop("backdrop2")
        INTRO.show()
        INTRO.size = 2  # scratch clamps "set size -100%" to a tiny sprite
        for _ in range(5):
            INTRO.size += 5
            yield
        for _ in range(4):
            INTRO.size += 20
            yield
        for _ in range(3):
            INTRO.size += 1
            yield
        yield from W(0.1)
        for _ in range(12):
            INTRO.size -= 1
            yield
        INTRO.size = 100
        yield from W(0.5)
        eng.broadcast("start button enable")
        while True:
            yield from INTRO.glide(0.5, 0, 5)
            yield from INTRO.glide(0.5, 0, 0)

    @eng.on("start game", INTRO)
    def _intro_out():
        eng.stop_other_scripts(INTRO)
        yield from INTRO.glide(0.3, 0, -298)
        INTRO.hide()

    @eng.on("start button enable", STARTBTN)
    def _btn_anim():
        STARTBTN.show()
        while True:
            STARTBTN.set_costume("costume1")
            yield from W(0.4)
            STARTBTN.set_costume("costume2")
            yield from W(0.4)

    @eng.on("start button enable", STARTBTN)
    def _btn_input():
        while True:
            if key("enter"):
                eng.stop_other_scripts(STARTBTN)
                STARTBTN.play("startbutton")
                STARTBTN.size = 70
                for _ in range(3):
                    STARTBTN.next_costume()
                    STARTBTN.size += 5
                    yield
                STARTBTN.size = 70
                for _ in range(5):
                    STARTBTN.set_costume("costume1")
                    yield from W(0.05)
                    STARTBTN.set_costume("costume2")
                    yield from W(0.05)
                STARTBTN.set_costume("costume1")
                yield from W(0.05)
                eng.broadcast("start game")
                STARTBTN.hide()
                return
            yield

    @eng.on("start game", PANEL)
    def _panel():
        yield from W(0.5)
        PANEL.show()
        PANEL.size = 100
        PANEL.set_costume("costume1")
        PANEL.goto(0, -394)
        yield from PANEL.glide(0.3, 0, 0)
        yield from W(1)
        PANEL.set_costume("costume2")
        while True:
            if key("enter"):
                eng.stop_other_scripts(PANEL)
                PANEL.goto(0, 0)
                PANEL.play("startbutton")
                for _ in range(5):
                    PANEL.set_costume("costume2")
                    yield from W(0.05)
                    PANEL.set_costume("costume1")
                    yield from W(0.05)
                PANEL.set_costume("costume2")
                yield from W(0.05)
                PANEL.goto(0, 0)
                yield from PANEL.glide(0.3, 0, -291)
                eng.broadcast("go to lobby")
                PANEL.hide()
                return
            yield

    # =========================================================================
    # White: full-screen fades
    # =========================================================================
    def _white_fade_out():
        WHITE.show()
        WHITE.front()
        WHITE.ghost = 0
        for _ in range(20):
            WHITE.ghost += 5
            yield
        WHITE.hide()

    @eng.on("PlayerEnable", WHITE)
    def _white_pe():
        yield from _white_fade_out()

    @eng.on("start intro", WHITE)
    def _white_si():
        yield from _white_fade_out()

    @eng.on("whitechange", WHITE)
    def _white_change():
        WHITE.show()
        WHITE.front()
        WHITE.ghost = 100
        for _ in range(10):
            WHITE.ghost -= 10
            yield
        WHITE.ghost = 0
        yield from W(0.05)
        for _ in range(20):
            WHITE.ghost += 5
            yield
        WHITE.hide()

    @eng.on("the end", WHITE)
    def _white_end():
        WHITE.set_costume("costume2")
        WHITE.show()
        WHITE.front()
        WHITE.ghost = 100
        for _ in range(10):
            WHITE.ghost -= 10
            yield
        yield from W(1)
        eng.broadcast("ending score")
        WHITE.ghost = 0
        yield from W(0.05)
        for _ in range(20):
            WHITE.ghost += 5
            yield
        WHITE.hide()
        WHITE.set_costume("costume1")

    # =========================================================================
    # Platform
    # =========================================================================
    @eng.on("go to lobby", PLAT)
    def _plat_lobby():
        PLAT.back()
        PLAT.show()
        PLAT.set_costume("costume2")
        PLAT.goto(0, -141)
        yield from PLAT.glide(0.5, 0, -50)
        eng.broadcast("PlayerEnable")

    @eng.on("level1", PLAT)
    def _plat_lv1():
        PLAT.show()
        PLAT.set_costume("costume3")
        PLAT.goto(0, -141)
        yield from PLAT.glide(0.2, 0, -50)

    @eng.on("startlv2", PLAT)
    def _plat_lv2():
        PLAT.hide()
        return
        yield

    @eng.on("level3", PLAT)
    def _plat_lv3():
        PLAT.show()
        PLAT.set_costume("costume5")
        PLAT.goto(0, -141)
        yield from PLAT.glide(0.2, 0, -50)

    @eng.on("ending cutscene", PLAT)
    def _plat_end():
        PLAT.show()
        PLAT.set_costume("costume1")
        PLAT.goto(0, -50)
        return
        yield

    # =========================================================================
    # Player (invisible physics hitbox; ANIM is the visible character)
    # =========================================================================
    def _touch_plat():
        return PLAYER.touching(PLAT, inset=0.0)

    @eng.on("PlayerEnable", PLAYER)
    def _player_physics():
        PLAYER.show()
        PLAYER.set_costume("char2")
        PLAYER.ghost = 99
        PLAYER.goto(-200, 30)
        PLAYER.sy = 0
        while True:
            dx = eng.kdir() * 5
            PLAYER.x += dx
            # Scratch stage fencing kept the player on screen
            PLAYER.x = max(-235, min(235, PLAYER.x))
            if _touch_plat():
                PLAYER.x -= dx
            PLAYER.sy -= 1
            PLAYER.y += PLAYER.sy
            if _touch_plat():
                PLAYER.y -= PLAYER.sy
                PLAYER.sy = 15 if (PLAYER.sy < 1 and key("z")) else 0
            yield

    @eng.on("RUNenable", PLAYER)
    def _player_run():
        # level 3: fixed x, jump only, stronger jump
        PLAYER.show()
        PLAYER.set_costume("char2")
        PLAYER.ghost = 99
        PLAYER.goto(90, -54)
        PLAYER.sy = 0
        while True:
            PLAYER.sy -= 1
            PLAYER.y += PLAYER.sy
            if _touch_plat():
                PLAYER.y -= PLAYER.sy
                PLAYER.sy = 17 if (PLAYER.sy < 1 and key("z")) else 0
            yield

    for _msg in ("turn anim", "oofie", "falloofie", "disableplayer",
                 "final cutscene", "ending cutscene", "stopmusic"):
        @eng.on(_msg, PLAYER)
        def _player_off():
            eng.stop_other_scripts(PLAYER)
            PLAYER.hide()
            return
            yield

    @eng.on("rightdash", PLAYER)
    def _player_rdash():
        PLAYER.play("slide")
        PLAYER.point(90)
        for step in (30, 30, 30, 15, 15, 15, 5, 5, 5):
            PLAYER.move_steps(step)
            yield

    @eng.on("leftdash", PLAYER)
    def _player_ldash():
        PLAYER.play("slide")
        PLAYER.point(-90)
        for step in (30, 30, 30, 15, 15, 15, 5, 5, 5):
            PLAYER.move_steps(step)
            yield

    # =========================================================================
    # CharacterAnim (visible Koki)
    # =========================================================================
    @eng.on("PlayerEnable", ANIM)
    def _anim_follow():
        while True:
            ANIM.goto_sprite(PLAYER)
            yield

    @eng.on("PlayerEnable", ANIM)
    def _anim_reset():
        ANIM.brightness = 0
        ANIM.ghost = 0
        ANIM.set_costume("costume10")
        ANIM.rotation_style = "left-right"
        ANIM.show()
        ANIM.set_costume("costume2")
        return
        yield

    @eng.on("PlayerEnable", ANIM)
    def _anim_walk():
        while True:
            moving = key("left") or key("right")
            if moving and not (key("z") or not ANIM.touching(PLAT)):
                while True:
                    if (not (key("left") or key("right"))) or key("z") or \
                       (key("left") and key("right")):
                        break
                    for c in ("costume6", "costume8", "costume4", "costume9"):
                        ANIM.set_costume(c)
                        yield from W(0.01)
                ANIM.set_costume("costume2")
                yield from W(0.05)
            yield

    @eng.on("PlayerEnable", ANIM)
    def _anim_jump():
        while True:
            if key("z") and not ANIM.touching(PLAT):
                while not ANIM.touching(PLAT):
                    ANIM.set_costume("costume10")
                    yield from W(0.01)
                ANIM.set_costume("costume11")
                yield from W(0.05)
                ANIM.set_costume("costume2")
                yield from W(0.01)
            yield

    @eng.on("PlayerEnable", ANIM)
    def _anim_idle():
        while True:
            yield from W(10)
            if not eng.input.any_key() and ANIM.touching(PLAT):
                ANIM.set_costume("costume3")
                yield from W(0.05)
                ANIM.set_costume("costume2")
                yield from W(0.01)

    @eng.on("PlayerEnable", ANIM)
    def _anim_face():
        while True:
            if key("right"):
                ANIM.point(90)
            if key("left"):
                ANIM.point(-90)
            yield

    @eng.on("idleanim", ANIM)
    def _anim_idle_set():
        ANIM.set_costume("costume2")
        return
        yield

    @eng.on("turn anim", ANIM)
    def _anim_door():
        eng.stop_other_scripts(ANIM)
        ANIM.set_costume("costume4")
        yield from W(0.1)
        ANIM.set_costume("door")
        yield from W(0.6)
        ANIM.set_costume("costume2")

    @eng.on("oofie", ANIM)
    def _anim_oofie():
        eng.stop_other_scripts(ANIM)
        ANIM.play("hit")
        ANIM.set_costume("OOF")
        ANIM.brightness = 0
        ANIM.ghost = 0
        yield from W(1)
        ANIM.play("Lose sound")
        for _ in range(7):
            ANIM.y += 5
            yield
        yield from ANIM.glide(0.7, ANIM.x, -204)
        ANIM.hide()
        yield from W(2)
        eng.broadcast("whitechange")
        yield from W(0.05)
        if V["lives"] <= 0:
            eng.broadcast("game over")
            return
        eng.broadcast("go to lobby")

    # gate 'take damage' through brief invincibility, then fan out as
    # 'koki hurt' (health bar + flash listen to that instead)
    @eng.on("take damage", None)
    def _dmg_gate():
        t = eng.now()
        if t - V.get("_hurt_t", -99) >= IFRAMES:
            V["_hurt_t"] = t
            eng.broadcast("koki hurt")
        return
        yield

    @eng.on("koki hurt", ANIM)
    def _anim_dmg_sound():
        ANIM.play("hit2")
        for _ in range(10):
            ANIM.set_costume("OOF")
            yield

    @eng.on("koki hurt", ANIM)
    def _anim_dmg_flash():
        for _ in range(10):
            ANIM.brightness = 50
            ANIM.ghost = 50
            yield from W(0.05)
            ANIM.brightness = 0
            ANIM.ghost = 0
            yield from W(0.05)

    @eng.on("dodge", ANIM)
    def _anim_dodge():
        ANIM.play("slide")
        ANIM.set_costume("costume11")
        for _ in range(10):
            ANIM.brightness = 50
            ANIM.ghost = 50
            yield from W(0.05)
            ANIM.brightness = 0
            ANIM.ghost = 0
            yield from W(0.05)

    @eng.on("disableplayer", ANIM)
    def _anim_disable():
        eng.stop_other_scripts(ANIM)
        ANIM.hide()
        return
        yield

    @eng.on("falloofie", ANIM)
    def _anim_fall():
        eng.stop_other_scripts(ANIM)
        ANIM.hide()
        ANIM.play("Fall")
        yield from W(1)
        ANIM.play("Lose sound")
        yield from W(2.7)
        eng.broadcast("whitechange")
        yield from W(0.05)
        if V["lives"] <= 0:
            eng.broadcast("game over")
            return
        eng.broadcast("go to lobby")

    # -- level 3 auto-run form ------------------------------------------------
    @eng.on("RUNenable", ANIM)
    def _anim_run_follow():
        ANIM.ghost = 0
        while True:
            ANIM.goto_sprite(PLAYER)
            yield

    @eng.on("RUNenable", ANIM)
    def _anim_run_start():
        ANIM.show()
        eng.broadcast("RUNNN")
        ANIM.point(90)
        return
        yield

    @eng.on("RUNNN", ANIM)
    def _anim_runnn_follow():
        while True:
            ANIM.goto_sprite(PLAYER)
            yield

    @eng.on("RUNNN", ANIM)
    def _anim_runnn_anim():
        ANIM.ghost = 0
        ANIM.brightness = 0
        while True:
            for c in ("costume6", "costume8", "costume4", "costume9"):
                ANIM.set_costume(c)
                yield from W(0.01)

    @eng.on("RUNNN", ANIM)
    def _anim_runnn_jump():
        while True:
            if key("z"):
                ANIM.ghost = 0
                ANIM.brightness = 0
                eng.stop_other_scripts(ANIM)
                eng.broadcast("jumpRUN")
                ANIM.set_costume("costume10")
                yield from W(1.1)
                ANIM.set_costume("costume11")
                yield from W(0.05)
                eng.stop_other_scripts(ANIM)
                eng.broadcast("RUNNN")
            yield

    @eng.on("jumpRUN", ANIM)
    def _anim_jumprun_follow():
        while True:
            ANIM.goto_sprite(PLAYER)
            yield

    # -- cutscene bits ----------------------------------------------------------
    @eng.on("final cutscene", ANIM)
    def _anim_finalcut():
        ANIM.y = -75
        ANIM.set_costume("door")
        yield from W(0.05)
        eng.broadcast("temporary hit")
        ANIM.play("hit2")
        ANIM.set_costume("OOF")
        yield from ANIM.glide(0.1, 135, -75)
        ANIM.set_costume("costume2")
        yield from W(1)

    @eng.on("cutscenehit", ANIM)
    def _anim_cuthit():
        ANIM.y = -75
        eng.broadcast("temporaryhit2")
        ANIM.play("hit2")
        ANIM.play("slide")
        ANIM.set_costume("costume11")
        ANIM.goto(135, -75)
        yield from ANIM.glide(0.2, -200, -75)
        ANIM.set_costume("OOF")
        yield from W(0.05)
        ANIM.set_costume("costume2")
        yield from W(1)

    @eng.on("enemy4 damage", ANIM)
    def _anim_en4dmg():
        ANIM.play("Fall2")
        ANIM.set_costume("door")
        yield from W(0.1)
        ANIM.set_costume("costume2")

    @eng.on("ending cutscene", ANIM)
    def _anim_ending_walk():
        ANIM.show()
        ANIM.goto(-243, -75)
        yield from ANIM.glide(20, 250, -75)
        eng.stop_other_scripts(ANIM)
        eng.broadcast("the end")
        ANIM.hide()

    @eng.on("ending cutscene", ANIM)
    def _anim_ending_anim():
        while True:
            for c in ("costume6", "costume8", "costume4", "costume9"):
                ANIM.set_costume(c)
                yield from W(0.1)

    @eng.on("stopmusic", ANIM)
    def _anim_lv3_dance():
        eng.stop_other_scripts(ANIM)
        ANIM.show()
        ANIM.goto(90, -75)
        for _ in range(3):
            ANIM.set_costume("costume11")
            yield from W(0.3)
            ANIM.set_costume("costume3")
            yield from W(0.3)

    # =========================================================================
    # Lives icon + Koki health bar + game over
    # =========================================================================
    @eng.on("go to lobby", LIVES)
    def _lives():
        if V["lives"] >= 3:
            LIVES.set_costume("costume1")
        elif V["lives"] == 2:
            LIVES.set_costume("costume2")
        elif V["lives"] == 1:
            LIVES.set_costume("costume3")
        yield from W(1)
        LIVES.show()

    @eng.on("doorinteracted", LIVES)
    def _lives_hide():
        LIVES.hide()
        return
        yield

    @eng.on("ending cutscene", LIVES)
    def _lives_hide2():
        LIVES.hide()
        return
        yield

    @eng.on("startlv1", KSTAT)
    def _kstat_lv1():
        KSTAT.set_costume("costume1")
        return
        yield

    @eng.on("PlayerEnable", KSTAT)
    def _kstat_in():
        eng.broadcast("restoreallhealth")
        KSTAT.show()
        KSTAT.goto(-308, -144)
        yield from KSTAT.glide(0.3, -150, -144)

    @eng.on("koki hurt", KSTAT)
    def _kstat_dmg_bounce():
        V["taken damage"] = 1
        yield from KSTAT.glide(0.05, -150, -139)
        yield from KSTAT.glide(0.1, -150, -144)

    @eng.on("koki hurt", KSTAT)
    def _kstat_dmg():
        KSTAT.next_costume()
        if KSTAT.costume_is("Oof"):
            eng.broadcast("oofie")
            V["knockouts"] += 1
            yield from W(0.05)
            KSTAT.hide()

    @eng.on("restoreallhealth", KSTAT)
    def _kstat_restore():
        KSTAT.set_costume("costume1")
        return
        yield

    @eng.on("start game", KSTAT)
    def _kstat_newgame():
        V["lives"] = 3
        return
        yield

    @eng.on("oofie", KSTAT)
    def _kstat_oofie():
        V["lives"] -= 1
        return
        yield

    @eng.on("falloofie", KSTAT)
    def _kstat_fall():
        V["lives"] -= 1
        KSTAT.hide()
        return
        yield

    @eng.on("startlv2", KSTAT)
    def _kstat_lv2():
        eng.stop_other_scripts(KSTAT)
        KSTAT.goto(-150, -144)
        yield from KSTAT.glide(0.3, -308, -144)
        KSTAT.hide()

    @eng.on("startlv3", KSTAT)
    def _kstat_lv3():
        eng.broadcast("restoreallhealth")
        KSTAT.show()
        KSTAT.goto(-308, -144)
        yield from KSTAT.glide(0.3, -150, -144)

    @eng.on("temporary hit", KSTAT)
    def _kstat_temphit():
        KSTAT.set_costume("costume2")
        yield from KSTAT.glide(0.05, -150, -139)
        yield from KSTAT.glide(0.1, -150, -144)

    @eng.on("temporaryhit2", KSTAT)
    def _kstat_temphit2():
        KSTAT.set_costume("costume3")
        yield from KSTAT.glide(0.05, -150, -139)
        yield from KSTAT.glide(0.1, -150, -144)

    @eng.on("partialrestore", KSTAT)
    def _kstat_partial():
        if KSTAT.costume_is("costume2"):
            KSTAT.set_costume("costume1")
        elif KSTAT.costume_is("costume3"):
            KSTAT.set_costume("costume2")
        elif KSTAT.costume_is("costume4"):
            KSTAT.set_costume("costume3")
        return
        yield

    @eng.on("ending cutscene", KSTAT)
    def _kstat_end():
        eng.stop_other_scripts(KSTAT)
        KSTAT.hide()
        return
        yield

    @eng.on("game over", GOVER)
    def _gameover_music():
        GOVER.music("KokiPrototypelOBBY")
        return
        yield

    @eng.on("game over", GOVER)
    def _gameover():
        GOVER.front()
        GOVER.show()
        GOVER.set_costume("costume1")
        yield from W(0.5)
        while True:
            GOVER.set_costume("costume2")
            if key("enter"):
                GOVER.play("startbutton")
                eng.stop_music()
                for _ in range(5):
                    GOVER.set_costume("costume1")
                    yield from W(0.05)
                    GOVER.set_costume("costume2")
                    yield from W(0.05)
                GOVER.set_costume("costume2")
                yield from W(0.05)
                eng.broadcast("whitechange")
                eng.broadcast("go to lobby")
                eng.broadcast("lockAlldoors")
                V["lives"] = 3
                GOVER.hide()
                eng.stop_other_scripts(GOVER)
                return
            yield

    # =========================================================================
    # Doors / lobby
    # =========================================================================
    def _door_interact(door, on_enter, unlock_check=None):
        """Shared door script body: wait, show if unlocked, watch for x/up."""
        door.hide()
        yield from W(0.5)
        if unlock_check is not None and not unlock_check():
            return
        door.show()
        while True:
            if door.touching(ANIM) and (key("x") or key("up")):
                on_enter()
                return
            yield

    def _door_flash(door):
        while True:
            door.set_costume("costume1")
            yield from W(0.3)
            door.set_costume("costume2")
            yield from W(0.3)

    # Door1 -> level 1
    @eng.on("go to lobby", DOOR1)
    def _door1_watch():
        def enter():
            eng.broadcast("turn anim")
            eng.broadcast("level1")
            eng.broadcast("doorinteracted")
        yield from _door_interact(DOOR1, enter)

    @eng.on("go to lobby", DOOR1)
    def _door1_flash():
        yield from _door_flash(DOOR1)

    @eng.on("level1", DOOR1)
    def _door1_go():
        eng.broadcast("whitechange")
        yield from W(0.05)
        eng.broadcast("startlv1")
        eng.broadcast("PlayerEnable")

    # Door2 -> level 2
    @eng.on("go to lobby", DOOR2)
    def _door2_watch():
        def enter():
            eng.broadcast("turn anim")
            eng.broadcast("level2")
            eng.broadcast("doorinteracted")
        yield from _door_interact(DOOR2, enter,
                                  unlock_check=lambda: V["doors"] >= 2)

    @eng.on("go to lobby", DOOR2)
    def _door2_flash():
        yield from _door_flash(DOOR2)

    @eng.on("level2", DOOR2)
    def _door2_go():
        eng.broadcast("whitechange")
        yield from W(0.05)
        eng.broadcast("startlv2")
        eng.broadcast("disableplayer")
        eng.broadcast("planecutscene")

    @eng.on("unlock door2", DOOR2)
    def _door2_unlock():
        V["doors"] = 2
        return
        yield

    @eng.on("lockAlldoors", DOOR2)
    def _doors_lock():
        V["doors"] = 1
        return
        yield

    # Door3 -> level 3
    @eng.on("go to lobby", DOOR3)
    def _door3_watch():
        def enter():
            eng.broadcast("turn anim")
            eng.broadcast("level3")
            eng.broadcast("doorinteracted")
        yield from _door_interact(DOOR3, enter,
                                  unlock_check=lambda: V["doors"] >= 3)

    @eng.on("go to lobby", DOOR3)
    def _door3_flash():
        yield from _door_flash(DOOR3)

    @eng.on("level3", DOOR3)
    def _door3_go():
        eng.broadcast("whitechange")
        yield from W(0.05)
        eng.broadcast("startlv3")
        eng.broadcast("RUNenable")

    # Door4 -> final level / ending
    @eng.on("go to lobby", DOOR4)
    def _door4_watch():
        def enter():
            eng.broadcast("turn anim")
            eng.broadcast("level 4")
        yield from _door_interact(DOOR4, enter,
                                  unlock_check=lambda: V["doors"] >= 4)

    @eng.on("go to lobby", DOOR4)
    def _door4_flash():
        yield from _door_flash(DOOR4)

    @eng.on("level 4", DOOR4)
    def _door4_go():
        eng.stop_other_scripts(DOOR4)
        eng.broadcast("final cutscene")
        DOOR4.show()
        yield from _door_flash(DOOR4)

    @eng.on("door4openagain", DOOR4)
    def _door4_again_flash():
        yield from _door_flash(DOOR4)

    @eng.on("door4openagain", DOOR4)
    def _door4_again_watch():
        while True:
            if DOOR4.touching(ANIM) and (key("x") or key("up")):
                eng.broadcast("turn anim")
                eng.broadcast("whitechange")
                yield from W(0.5)
                eng.broadcast("ending cutscene")
                return
            yield

    # doors hide on level start / cutscenes
    def _mk_hide(spr, stop=False):
        def _h():
            if stop:
                eng.stop_other_scripts(spr)
            spr.hide()
            return
            yield
        return _h

    for _d in (DOOR1, DOOR2, DOOR3, DOOR4):
        eng.on("doorinteracted", _d)(_mk_hide(_d))
        eng.on("level2", _d)(_mk_hide(_d))
    for _d in (DOOR1, DOOR2, DOOR3):
        eng.on("level3", _d)(_mk_hide(_d))
    eng.on("level3", DOOR4)(_mk_hide(DOOR4))
    for _d in (DOOR1, DOOR2, DOOR3, DOOR4):
        eng.on("ending cutscene", _d)(_mk_hide(_d, stop=True))

    # during the final cutscene the other doors flash decoratively
    for _d in (DOOR1, DOOR2, DOOR3):
        def _mk_cut(door):
            def _h():
                eng.stop_other_scripts(door)
                door.show()
                yield from _door_flash(door)
            return _h
        eng.on("final cutscene", _d)(_mk_cut(_d))

        def _mk_still(door):
            def _h():
                eng.stop_other_scripts(door)
                door.set_costume("costume1")
                return
                yield
            return _h
        eng.on("startfinallevel", _d)(_mk_still(_d))
    eng.on("startfinallevel", DOOR4)(_mk_still(DOOR4))

    # =========================================================================
    # Level 1: Enemy 1 boss
    # =========================================================================
    def _en1_shockwave_volley():
        for _ in range(3):
            EN1.set_costume("costume3")
            yield from W(0.05)
            EN1.set_costume("costume4")
            yield from W(0.05)
            EN1.set_costume("costume5")
            yield from W(0.05)
            eng.broadcast("shockwave")
            for _ in range(2):
                EN1.set_costume("costume10")
                yield from W(0.05)
                EN1.play("explbomb")
                EN1.set_costume("costume6")
                yield from W(0.05)
                EN1.set_costume("costume5")
                yield from W(0.2)
            EN1.set_costume("costume3")
            yield from W(0.05)
            EN1.set_costume("costume2")
            yield from W(0.05)
            EN1.set_costume("costume1")
            yield from W(0.1)

    def _en1_idle_5():
        for _ in range(5):
            EN1.set_costume("costume1")
            yield from W(0.3)
            EN1.set_costume("costume2")
            yield from W(0.3)

    @eng.on("startlv1", EN1)
    def _en1_start():
        EN1.goto(173, -48)
        EN1.show()
        EN1.clear_fx()
        yield from _en1_idle_5()
        yield from _en1_shockwave_volley()
        eng.broadcast("enemy1 idle")
        yield from W(0.2)
        eng.broadcast("spawncanon")

    @eng.on("startlv1", EN1)
    def _en1_bell():
        yield from W(2)
        eng.broadcast("boxing bell")
        eng.broadcast("hitbox")

    @eng.on("hitbox", EN1)
    def _en1_hitbox():
        while True:
            if EN1.touching(ANIM):
                eng.broadcast("take damage")
                return
            yield

    @eng.on("take damage", EN1)
    def _en1_rearm():
        yield from W(1)
        eng.broadcast("hitbox")

    @eng.on("enemy1 idle", EN1)
    def _en1_idle():
        while True:
            EN1.set_costume("costume1")
            yield from W(0.3)
            EN1.set_costume("costume2")
            yield from W(0.3)

    @eng.on("enemy1damage", EN1)
    def _en1_damaged():
        eng.stop_other_scripts(EN1)
        EN1.play("output (24)")
        EN1.set_costume("OOF")
        yield from W(0.5)
        EN1.set_costume("costume1")
        yield from W(0.5)
        eng.broadcast("enemy1 pounce")

    @eng.on("enemy1 pounce", EN1)
    def _en1_pounce():
        EN1.set_costume("costume7")
        EN1.play("output (24)2")
        eng.broadcast("quickhitbox")
        eng.broadcast("quick tap")
        yield from EN1.glide(0.3 * ATK, 0, 94)
        yield from EN1.glide_to_sprite(0.3 * ATK, ANIM)
        EN1.play("explbomb")
        EN1.set_costume("costume8")
        EN1.y = -48
        yield from W(0.05)
        EN1.set_costume("costume9")
        yield from W(0.2)
        eng.stop_other_scripts(EN1)
        EN1.set_costume("costume1")
        yield from EN1.glide(0.3, 173, -48)
        eng.broadcast("hitbox")
        eng.broadcast("phaseagain")

    @eng.on("phaseagain", EN1)
    def _en1_phaseagain():
        yield from _en1_idle_5()
        yield from _en1_shockwave_volley()
        eng.broadcast("enemy1 idle")
        yield from W(0.2)
        eng.broadcast("spawncanon")

    @eng.on("quickhitbox", EN1)
    def _en1_quickhitbox():
        while True:
            if EN1.touching(ANIM):
                if key("x"):
                    eng.broadcast("dodge")
                else:
                    eng.broadcast("take damage")
                return
            yield

    @eng.on("oofie", EN1)
    def _en1_playerdead():
        eng.stop_other_scripts(EN1)
        return
        yield

    @eng.on("go to lobby", EN1)
    def _en1_lobby():
        eng.stop_other_scripts(EN1)
        EN1.hide()
        return
        yield

    @eng.on("enemy1 oof", EN1)
    def _en1_dead():
        eng.stop_other_scripts(EN1)
        EN1.play("explbomb2")
        EN1.set_costume("OOF")
        for _ in range(10):
            EN1.y += 5
            yield
        EN1.set_costume("OOF2")
        yield from EN1.glide(0.6, EN1.x, -48)
        EN1.ghost = 0
        for _ in range(20):
            EN1.ghost += 5
            yield
        EN1.hide()
        yield from W(1)
        eng.broadcast("whitechange")
        yield from W(0.05)
        eng.broadcast("unlock door2")
        eng.broadcast("go to lobby")

    # -- final-boss possession phase (Enemy 1 returns, purple) ----------------
    @eng.on("en1final", EN1)
    def _en1_final_entry():
        EN1.show()
        EN1.clear_fx()
        EN1.goto(265, 109)
        yield from EN1.glide(0.1, 84, -48)
        EN1.play("explbomb")
        EN1.set_costume("costume8")
        yield from W(0.05)
        EN1.set_costume("costume9")
        yield from W(0.2)
        eng.stop_other_scripts(EN1)
        EN1.set_costume("costume1")
        yield from _en1_idle_5()

    @eng.on("possessen1", EN1)
    def _en1_possessed():
        eng.stop_other_scripts(EN1)
        EN1.play("output (24)")
        EN1.set_costume("OOF")
        yield from W(0.5)
        EN1.set_costume("costume1")
        # brightness dip = the "possessed" purple tint (no color fx on port)
        EN1.brightness = 0
        for _ in range(6):
            EN1.brightness -= 25
            yield from W(0.05)
        for _ in range(4):
            EN1.brightness += 25
            yield
        EN1.brightness = -50   # stays dark while possessed
        yield from _en1_idle_5()
        while True:
            EN1.set_costume("costume7")
            EN1.play("output (24)2")
            eng.broadcast("quick tap")
            eng.broadcast("quickhitbox")
            yield from EN1.glide(0.3 * ATK, 0, 94)
            yield from EN1.glide_to_sprite(0.3 * ATK, ANIM)
            EN1.play("explbomb")
            EN1.set_costume("costume8")
            EN1.y = -48
            yield from W(0.05)
            EN1.set_costume("costume9")
            yield from W(0.2)
            EN1.set_costume("costume1")
            yield from EN1.glide(0.3, 173, -48)
            eng.broadcast("activehitonen1")
            yield from _en1_idle_5()
            yield from _en1_shockwave_volley()
            eng.broadcast("activehitonen1")
            yield from _en1_idle_5()

    @eng.on("activehitonen1", EN1)
    def _en1_activehit():
        while True:
            if EN1.touching(ANIM) and key("x"):
                eng.stop_other_scripts(EN1)
                EN1.play("explbomb2")
                EN1.set_costume("OOF")
                for _ in range(10):
                    EN1.y += 5
                    yield
                EN1.set_costume("OOF2")
                yield from EN1.glide(0.6, EN1.x, -48)
                eng.broadcast("RibyOUT")
                EN1.ghost = 0
                EN1.brightness = 0
                for _ in range(20):
                    EN1.ghost += 5
                    yield
                EN1.hide()
                yield from W(1)
                return
            yield

    # -- shockwaves (level 1) ---------------------------------------------------
    def _mk_shockwave(spr, delay):
        def _move():
            if delay:
                yield from W(delay)
            spr.show()
            spr.goto_sprite(EN1)
            yield from spr.glide(1 * ATK, -241, -45)
            eng.stop_other_scripts(spr)
            spr.hide()
        def _hit():
            if delay:
                yield from W(delay)
            while True:
                if spr.touching(ANIM):
                    eng.broadcast("take damage")
                    return
                yield
        return _move, _hit

    _sw1_move, _sw1_hit = _mk_shockwave(SW1, 0)
    _sw2_move, _sw2_hit = _mk_shockwave(SW2, 0.2)
    eng.on("shockwave", SW1)(_sw1_move)
    eng.on("shockwave", SW1)(_sw1_hit)
    eng.on("shockwave", SW2)(_sw2_move)
    eng.on("shockwave", SW2)(_sw2_hit)

    # -- Enemy 1 health bar -------------------------------------------------------
    @eng.on("startlv1", E1STAT)
    def _e1stat_in():
        eng.broadcast("restoreallhealth")
        E1STAT.show()
        E1STAT.set_costume("costume1")
        E1STAT.goto(308, -144)
        yield from E1STAT.glide(0.3, 150, -144)

    @eng.on("enemy1damage", E1STAT)
    def _e1stat_bounce():
        yield from E1STAT.glide(0.05, 150, -139)
        yield from E1STAT.glide(0.1, 150, -144)

    @eng.on("enemy1damage", E1STAT)
    def _e1stat_dmg():
        for _ in range(2):
            E1STAT.next_costume()
            yield from W(0.05)
        if E1STAT.costume_is("Oof2"):
            eng.broadcast("enemy1 oof")

    @eng.on("go to lobby", E1STAT)
    def _e1stat_out():
        E1STAT.goto(150, -144)
        yield from E1STAT.glide(0.3, 308, -144)
        E1STAT.hide()

    @eng.on("game over", E1STAT)
    def _e1stat_go():
        E1STAT.hide()
        return
        yield

    # -- Cannon + cannonball (level 1) ---------------------------------------------
    def _flash(spr, times=2):
        for _ in range(times):
            spr.ghost = 50
            spr.brightness = 50
            yield from W(0.05)
            spr.ghost = 0
            spr.brightness = 0
            yield from W(0.05)

    @eng.on("spawncanon", CANNON)
    def _cannon_flash():
        CANNON.show()
        yield from _flash(CANNON)

    @eng.on("spawncanon", CANNON)
    def _cannon_arm():
        CANNON.play("recording1")
        CANNON.goto(-79, -69)
        yield from CANNON.glide(0.3, -79, -74)
        while True:
            if key("x") and CANNON.touching(ANIM):
                eng.broadcast("canonball")
                return
            yield

    @eng.on("canonball", CANNON)
    def _cannon_fired():
        CANNON.play("explosion meme")
        yield from _flash(CANNON)
        yield from W(0.5)
        CANNON.hide()

    @eng.on("go to lobby", CANNON)
    def _cannon_lobby():
        eng.stop_other_scripts(CANNON)
        CANNON.hide()
        return
        yield

    @eng.on("canonball", CBALL)
    def _cball_fly():
        CBALL.goto(-41, -62)
        yield from CBALL.glide_to_sprite(0.7, EN1)
        eng.broadcast("enemy1damage")
        CBALL.hide()

    @eng.on("canonball", CBALL)
    def _cball_show():
        CBALL.show()
        CBALL.back()
        yield from _flash(CBALL)

    @eng.on("canonball", CBALL)
    def _cball_friendly_fire():
        while True:
            if CBALL.touching(ANIM, inset=0.3):
                eng.broadcast("take damage")
                return
            yield

    @eng.on("canonball2", CBALL)
    def _cball2_show():
        CBALL.show()
        CBALL.back()
        yield from _flash(CBALL)

    @eng.on("canonball2", CBALL)
    def _cball2_fly():
        CBALL.goto_sprite(CANNON2)
        yield from CBALL.glide_to_sprite(0.7, EN2)
        eng.broadcast("enemy2 damage")
        CBALL.hide()

    @eng.on("cannonball3", CBALL)
    def _cball3_show():
        CBALL.show()
        CBALL.back()
        yield from _flash(CBALL)

    @eng.on("cannonball3", CBALL)
    def _cball3_fly():
        CBALL.goto_sprite(CANNON3)
        yield from CBALL.glide_to_sprite(0.4, EN3)
        eng.broadcast("enemy 3 damage")
        CBALL.hide()

    @eng.on("go to lobby", CBALL)
    def _cball_lobby():
        CBALL.hide()
        return
        yield

    # Kill any in-flight ball when the player dies: otherwise its damage
    # broadcast lands ~0.7 s later, restarting the boss over the corpse
    # (or granting door progression on a death).
    for _msg in ("oofie", "planeoofie", "falloofie"):
        @eng.on(_msg, CBALL)
        def _cball_playerdead():
            eng.stop_other_scripts(CBALL)
            CBALL.hide()
            return
            yield

    # -- QuickPress prompt ------------------------------------------------------
    @eng.on("quick tap", QUICK)
    def _quick_input():
        QUICK.set_costume("costume1")
        while True:
            if key("x"):
                eng.stop_other_scripts(QUICK)
                # deviation: original always dashed right; dash AWAY from
                # the boss so the escape never lunges into him
                eng.broadcast("leftdash" if ANIM.x < EN1.x else "rightdash")
                QUICK.hide()
                return
            yield

    @eng.on("quick tap", QUICK)
    def _quick_show():
        QUICK.set_costume("costume1")
        for _ in range(4):
            QUICK.show()
            yield from W(0.05)
            QUICK.hide()
            yield from W(0.05)
        eng.stop_other_scripts(QUICK)

    @eng.on("Chase for the door", QUICK)
    def _quick_chase():
        QUICK.set_costume("costume2")
        for _ in range(6):
            QUICK.show()
            yield from W(0.05)
            QUICK.hide()
            yield from W(0.05)

    @eng.on("startfinallevel", QUICK)
    def _quick_final():
        QUICK.set_costume("costume3")
        for _ in range(5):
            QUICK.show()
            yield from W(0.2)
            QUICK.hide()
            yield from W(0.2)

    # =========================================================================
    # Level 2: plane vs dragon
    # =========================================================================
    @eng.on("planecutscene", CUT1)
    def _cut1_fall():
        yield from W(0.5)
        CUT1.show()
        CUT1.back()
        CUT1.set_costume("falling")
        CUT1.goto(-187, 180)
        yield from CUT1.glide(0.5, -187, 0)
        eng.broadcast("startplane")
        eng.broadcast("enableplane")
        CUT1.hide()

    @eng.on("planecutscene", PLANE)
    def _plane_in():
        PLANE.show()
        PLANE.goto(-332, 0)
        yield from PLANE.glide(1, -195, 0)

    @eng.on("planecutscene", PLANE)
    def _plane_prop():
        for _ in range(10):
            PLANE.set_costume("costume1")
            yield from W(0.01)
            PLANE.set_costume("costume3")
            yield from W(0.01)
        eng.broadcast("planeANIM")

    @eng.on("planeANIM", PLANE)
    def _plane_anim():
        while True:
            PLANE.set_costume("costume2")
            yield from W(0.01)
            PLANE.set_costume("costume5")
            yield from W(0.01)

    @eng.on("enableplane", PLANE)
    def _plane_up():
        while True:
            while key("up"):
                PLANE.y += 7
                yield
            yield

    @eng.on("enableplane", PLANE)
    def _plane_down():
        while True:
            while key("down"):
                PLANE.y -= 7
                yield
            yield

    @eng.on("enableplane", PLANE)
    def _plane_bounds():
        while True:
            if PLANE.y < -70:
                PLANE.y += 7
            if PLANE.y > 180:
                PLANE.y -= 7
            yield

    @eng.on("enableplane", PLANE)
    def _plane_boost():
        # deviation: original required z+arrow chords, which single-key
        # keypads (and ghosting keyboards) can't produce. Z boosts in the
        # last vertical direction tapped within 2s (double-tap-dash style);
        # with no recent tap it boosts toward open space. Chords still work.
        last_dir = "up"
        last_t = -99.0
        while True:
            if key("up"):
                last_dir, last_t = "up", eng.now()
            if key("down"):
                last_dir, last_t = "down", eng.now()
            if key("z"):
                if eng.now() - last_t <= 2.0:
                    boost = last_dir
                else:
                    boost = "up" if PLANE.y < 55 else "down"
                PLANE.play("slide")
                dy = 20 if boost == "up" else -20
                for _ in range(6):
                    PLANE.y += dy
                    yield
                eng.broadcast("rechargeeffect")
                yield from W(1)
            yield

    @eng.on("rechargeeffect", PLANE)
    def _plane_recharge():
        yield from _flash(PLANE, 5)

    @eng.on("takeplanedamage", None)
    def _plane_dmg_gate():
        t = eng.now()
        if t - V.get("_plane_hurt_t", -99) >= IFRAMES:
            V["_plane_hurt_t"] = t
            eng.broadcast("plane hurt")
        return
        yield

    @eng.on("plane hurt", PLANE)
    def _plane_hit():
        eng.stop_other_scripts(PLANE)
        eng.broadcast("enableplane")
        PLANE.play("hit sound")
        for c in ("costume4", "costume6", "costume9", "costume10"):
            PLANE.set_costume(c)
            yield from W(0.05)
        PLANE.play("beep")
        for _ in range(8):
            PLANE.set_costume("costume7")
            yield from W(0.01)
            PLANE.set_costume("costume8")
            yield from W(0.01)
        PLANE.ghost = 0
        eng.broadcast("planeANIM")

    @eng.on("plane hurt", PLANE)
    def _plane_hit_flash():
        yield from _flash(PLANE, 10)

    @eng.on("planeoofie", PLANE)
    def _plane_dead():
        eng.stop_other_scripts(PLANE)
        PLANE.play("hit sound")
        PLANE.play("hit")
        eng.broadcast("planeoofanim")
        yield from W(1)
        PLANE.play("Lose sound")
        yield from PLANE.glide(1, 37, -246)
        PLANE.play("explbomb2")
        PLANE.hide()
        yield from W(2)
        eng.broadcast("whitechange")
        yield from W(0.05)
        if V["lives"] <= 0:
            eng.broadcast("game over")
            return
        eng.broadcast("go to lobby")

    @eng.on("planeoofanim", PLANE)
    def _plane_oofanim():
        while True:
            PLANE.set_costume("Oofie")
            yield from W(0.01)
            PLANE.set_costume("Oofie2")
            yield from W(0.01)

    @eng.on("go to lobby", PLANE)
    def _plane_lobby():
        eng.stop_other_scripts(PLANE)
        PLANE.hide()
        return
        yield

    # -- plane health bar -----------------------------------------------------
    @eng.on("startlv2", KPSTAT)
    def _kpstat_reset():
        KPSTAT.set_costume("costume1")
        return
        yield

    @eng.on("startplane", KPSTAT)
    def _kpstat_in():
        KPSTAT.show()
        KPSTAT.goto(-308, -144)
        yield from KPSTAT.glide(0.3, -150, -144)
        eng.broadcast("restoreplane")

    @eng.on("plane hurt", KPSTAT)
    def _kpstat_bounce():
        V["taken damage"] += 1
        yield from KPSTAT.glide(0.05, -150, -139)
        yield from KPSTAT.glide(0.1, -150, -144)

    @eng.on("restoreplane", KPSTAT)
    def _kpstat_restore():
        KPSTAT.set_costume("costume1")
        return
        yield

    @eng.on("plane hurt", KPSTAT)
    def _kpstat_dmg():
        KPSTAT.next_costume()
        yield from W(0.05)
        if KPSTAT.costume_is("Oof"):
            eng.broadcast("planeoofie")
            V["knockouts"] += 1
            yield from W(0.05)
            KPSTAT.hide()

    @eng.on("planeoofie", KPSTAT)
    def _kpstat_oof():
        V["lives"] -= 1
        return
        yield

    @eng.on("go to lobby", KPSTAT)
    def _kpstat_out():
        KPSTAT.goto(-150, -144)
        yield from KPSTAT.glide(0.3, -308, -144)
        KPSTAT.hide()

    # -- Enemy2 (dragon) --------------------------------------------------------
    @eng.on("startlv2", EN2)
    def _en2_entry():
        EN2.show()
        EN2.set_costume("costume1")
        EN2.goto(170, 227)
        yield from EN2.glide(1, 170, 0)
        yield from W(0.2)
        EN2.set_costume("costume2")
        yield from W(0.05)
        EN2.set_costume("costume3")
        yield from W(0.05)
        EN2.play("grrr")
        for _ in range(5):
            EN2.set_costume("costume4")
            yield from W(0.05)
            EN2.set_costume("costume5")
            yield from W(0.05)
        EN2.set_costume("costume3")
        yield from W(0.05)
        EN2.set_costume("costume2")
        yield from W(0.05)
        EN2.set_costume("costume1")
        yield from W(0.05)
        eng.broadcast("en2 cycle 1")

    @eng.on("hitbox2", EN2)
    def _en2_hitbox():
        while True:
            if EN2.touching(PLANE):
                eng.broadcast("takeplanedamage")
                return
            yield

    def _en2_attack(track_secs, track_reps, pace):
        """One tongue attack: wind up, track plane, lash out and retract."""
        eng.broadcast("hitbox2")
        EN2.set_costume("costume6")
        yield from W(pace)
        for _ in range(5):
            EN2.next_costume()
            yield from W(pace)
        eng.broadcast("enemy bright")
        for _ in range(track_reps):
            yield from EN2.glide(track_secs, 170, PLANE.y)
        EN2.play("output (27)")
        EN2.set_costume("costume12")
        yield from W(0.05)
        for _ in range(10):
            EN2.next_costume()
            yield from W(0.05)
        for c in ("costume11", "costume10", "costume9", "costume8",
                  "costume7"):
            EN2.set_costume(c)
            yield from W(0.05)
        EN2.set_costume("costume6")
        yield from W(0.2)

    @eng.on("en2 cycle 1", EN2)
    def _en2_cycle1():
        while True:
            for _ in range(4):
                yield from _en2_attack(0.1, 5, 0.05)
            eng.broadcast("spawncanon222")
            yield from W(1)

    @eng.on("en2 cycle 2", EN2)
    def _en2_cycle2():
        while True:
            for _ in range(4):
                yield from _en2_attack(0.05, 1, 0.03)
            eng.broadcast("spawncanon222")
            yield from W(0.5)

    @eng.on("enemy bright", EN2)
    def _en2_bright():
        yield from _flash(EN2, 6)

    @eng.on("planeoofie", EN2)
    def _en2_playerdead():
        eng.stop_other_scripts(EN2)
        return
        yield

    @eng.on("enemy2 damage", EN2)
    def _en2_damaged():
        eng.stop_other_scripts(EN2)
        eng.broadcast("enemy bright")
        EN2.play("output (24)")
        EN2.play("output (31)")
        EN2.set_costume("Damage")
        yield from W(0.05)
        for _ in range(9):
            EN2.next_costume()
            yield from W(0.05)
        EN2.play("grrr")
        for _ in range(5):
            EN2.set_costume("costume5")
            yield from W(0.05)
            EN2.set_costume("costume4")
            yield from W(0.05)
        EN2.set_costume("costume3")
        yield from W(0.05)
        EN2.set_costume("costume2")
        yield from W(0.05)
        EN2.set_costume("costume1")
        yield from W(0.05)
        eng.broadcast("ping enemy2 stats")

    @eng.on("enemy2 damage", EN2)
    def _en2_dmg_flash():
        yield from _flash(EN2, 10)

    @eng.on("enemy2 oof", EN2)
    def _en2_dead():
        eng.stop_other_scripts(EN2)
        EN2.clear_fx()
        EN2.set_costume("OOF")
        EN2.play("output (29)")
        EN2.play("output (28)")
        for _ in range(15):
            EN2.goto(R(-240, 240), R(-180, 180))
            yield
        EN2.play("output (30)")
        EN2.set_costume("OOF2")
        yield from W(0.05)
        EN2.set_costume("OOF3")
        yield from W(0.05)
        EN2.hide()
        V["doors"] = 3
        yield from W(3)
        eng.broadcast("go to lobby")

    @eng.on("go to lobby", EN2)
    def _en2_lobby():
        eng.stop_other_scripts(EN2)
        EN2.hide()
        return
        yield

    # -- Enemy2 health bar -------------------------------------------------------
    @eng.on("startlv2", E2STAT)
    def _e2stat_in():
        eng.broadcast("restoreplane")
        E2STAT.show()
        E2STAT.front()
        E2STAT.set_costume("costume1")
        E2STAT.goto(308, -144)
        yield from E2STAT.glide(0.3, 150, -144)

    @eng.on("enemy2 damage", E2STAT)
    def _e2stat_bounce():
        yield from E2STAT.glide(0.05, 150, -139)
        yield from E2STAT.glide(0.1, 150, -144)

    @eng.on("enemy2 damage", E2STAT)
    def _e2stat_dmg():
        E2STAT.next_costume()
        yield from W(0.05)
        if E2STAT.costume_is("Oof2"):
            eng.broadcast("enemy2 oof")

    @eng.on("ping enemy2 stats", E2STAT)
    def _e2stat_ping():
        if E2STAT.costume_number >= 5:
            eng.broadcast("en2 cycle 2")
        else:
            eng.broadcast("en2 cycle 1")
        return
        yield

    @eng.on("go to lobby", E2STAT)
    def _e2stat_out():
        E2STAT.goto(150, -144)
        yield from E2STAT.glide(0.3, 308, -144)
        E2STAT.hide()

    @eng.on("game over", E2STAT)
    def _e2stat_go():
        E2STAT.hide()
        return
        yield

    # -- Cannon2 / gas tank -----------------------------------------------------
    @eng.on("spawncanon222", CANNON2)
    def _cannon2_flash():
        CANNON2.show()
        yield from _flash(CANNON2)

    @eng.on("spawncanon222", CANNON2)
    def _cannon2_arm():
        CANNON2.play("recording1")
        while True:
            if key("x") and CANNON2.touching(PLANE):
                eng.broadcast("canonball2")
                return
            yield

    @eng.on("spawncanon222", CANNON2)
    def _cannon2_drop():
        CANNON2.play("recording1")
        CANNON2.show()
        CANNON2.goto(-100, 186)
        yield from CANNON2.glide(1.5, -100, -193)
        eng.stop_other_scripts(CANNON2)
        CANNON2.hide()

    @eng.on("canonball2", CANNON2)
    def _cannon2_fired():
        CANNON2.play("explosion meme")
        yield from _flash(CANNON2)

    @eng.on("go to lobby", CANNON2)
    def _cannon2_lobby():
        eng.stop_other_scripts(CANNON2)
        CANNON2.hide()
        return
        yield

    @eng.on("en2 cycle 2", GAS)
    def _gas_trigger():
        eng.broadcast("spawnfuel")
        return
        yield

    @eng.on("spawnfuel", GAS)
    def _gas_fly():
        GAS.show()
        GAS.goto(242, R(-88, 150))
        yield from GAS.glide(1.5, -243, GAS.y)
        eng.stop_other_scripts(GAS)
        GAS.hide()

    @eng.on("spawnfuel", GAS)
    def _gas_touch():
        while True:
            if GAS.touching(PLANE):
                eng.broadcast("restoreplane")
                GAS.play("heal")
                V["has healed"] = 1
                eng.stop_other_scripts(GAS)
                GAS.hide()
                return
            yield

    @eng.on("planeoofie", GAS)
    def _gas_off():
        eng.stop_other_scripts(GAS)
        GAS.hide()
        return
        yield

    @eng.on("go to lobby", GAS)
    def _gas_lobby():
        eng.stop_other_scripts(GAS)
        GAS.hide()
        return
        yield

    # "A to dodge" hint banner on entering level 2
    @eng.on("level2", DODGE)
    def _dodge_hint():
        DODGE.show()
        DODGE.front()
        for _ in range(5):
            DODGE.set_costume("costume1")
            yield from W(0.1)
            DODGE.set_costume("costume2")
            yield from W(0.1)
        DODGE.hide()

    # =========================================================================
    # Level 3: Popi chase
    # =========================================================================
    @eng.on("startlv3", EN3)
    def _en3_place():
        EN3.show()
        EN3.goto(-169, -55)
        return
        yield

    @eng.on("startlv3", EN3)
    def _en3_hitbox_arm():
        yield from W(2)
        eng.broadcast("hitbox")

    @eng.on("hitbox", EN3)
    def _en3_hitbox():
        while True:
            if EN3.touching(ANIM):
                eng.broadcast("take damage")
                return
            yield

    @eng.on("take damage", EN3)
    def _en3_rearm():
        yield from W(1)
        eng.broadcast("hitbox")

    @eng.on("startlv3", EN3)
    def _en3_run_start():
        eng.broadcast("enemy3 run")
        return
        yield

    @eng.on("enemy3 run", EN3)
    def _en3_run():
        while True:
            for c in ("costume7", "costume6", "costume5", "costume4"):
                EN3.set_costume(c)
                yield from W(0.01)

    @eng.on("startlv3", EN3)
    def _en3_begin_atk():
        EN3.brightness = 0
        V["cannondefeats"] = 1
        yield from W(3)
        eng.broadcast("en3atk")

    @eng.on("en3atk", EN3)
    def _en3_attack():
        while True:
            for _ in range(5):
                EN3.play("output (24)2")
                EN3.brightness = 0
                for _ in range(10):
                    EN3.brightness += 10
                    yield from W(0.05)
                EN3.play("recording2")
                eng.broadcast("shootenemyball")
                EN3.brightness -= 50
                yield from W(0.05)
                EN3.brightness -= 50
                yield from W(0.05)
                EN3.brightness = 0
                yield from _flash(EN3, 5)
            eng.broadcast("spawncannon333")
            yield from W(3)

    @eng.on("enemy 3 damage", EN3)
    def _en3_damaged():
        eng.stop_other_scripts(EN3)
        eng.broadcast("enemy3hit")
        EN3.play("output (24)")
        yield from _flash(EN3, 10)

    @eng.on("enemy3hit", EN3)
    def _en3_hit_anim():
        for c in ("costume8", "costume9", "costume10", "costume11",
                  "costume12", "costume4"):
            EN3.set_costume(c)
            yield from W(0.05)
        eng.broadcast("enemy3 run")
        yield from W(1)
        eng.broadcast("en3atk")

    @eng.on("enemy 3 oof", EN3)
    def _en3_phase_down():
        eng.stop_other_scripts(EN3)
        V["cannondefeats"] += 1
        EN3.play("output (24)")
        for c in ("costume8", "costume9", "costume10", "costume11",
                  "costume12"):
            EN3.set_costume(c)
            yield from W(0.05)
        EN3.goto(-169, -55)
        yield from EN3.glide(0.2, -293, -55)
        EN3.hide()
        if V["cannondefeats"] in (2, 3):
            eng.broadcast("Chase for the door")
            eng.broadcast("startshockwaves333")
            eng.broadcast("enemy3 run")
            eng.broadcast("restoreEN3")
            EN3.show()
            EN3.goto(-293, -55)
            yield from EN3.glide(1, -169, -55)
            yield from W(1)
            eng.broadcast("en3atk")
        elif V["cannondefeats"] == 4:
            eng.broadcast("stopmusic")
            yield from W(3)
            V["doors"] = 4
            eng.broadcast("go to lobby")

    @eng.on("oofie", EN3)
    def _en3_playerdead():
        eng.stop_other_scripts(EN3)
        return
        yield

    @eng.on("go to lobby", EN3)
    def _en3_lobby():
        eng.stop_other_scripts(EN3)
        EN3.hide()
        return
        yield

    # -- Popi's projectiles / cannon / shockwaves / abyss ------------------------
    @eng.on("shootenemyball", CBALL2)
    def _eball_fly():
        CBALL2.goto(-122, -41)
        yield from CBALL2.glide(0.2 * ATK, 250, -41)
        CBALL2.hide()

    @eng.on("shootenemyball", CBALL2)
    def _eball_show():
        CBALL2.show()
        CBALL2.back()
        yield from _flash(CBALL2)

    @eng.on("shootenemyball", CBALL2)
    def _eball_hit():
        while True:
            if CBALL2.touching(ANIM):
                eng.broadcast("take damage")
                CBALL2.hide()
                eng.stop_other_scripts(CBALL2)
                return
            yield

    @eng.on("go to lobby", CBALL2)
    def _eball_lobby():
        CBALL2.hide()
        return
        yield

    @eng.on("spawncannon333", CANNON3)
    def _cannon3_flash():
        CANNON3.show()
        yield from _flash(CANNON3)

    @eng.on("spawncannon333", CANNON3)
    def _cannon3_arm():
        CANNON3.play("recording1")
        while True:
            if key("x") and CANNON3.touching(ANIM):
                eng.broadcast("cannonball3")
                return
            yield

    @eng.on("spawncannon333", CANNON3)
    def _cannon3_slide():
        CANNON3.goto(262, -72)
        yield from CANNON3.glide(2, -263, -72)
        eng.stop_other_scripts(CANNON3)
        CANNON3.hide()

    @eng.on("cannonball3", CANNON3)
    def _cannon3_fired():
        CANNON3.play("explosion meme")
        yield from _flash(CANNON3)
        yield from W(0.5)
        CANNON3.hide()

    @eng.on("hidecannon3", CANNON3)
    def _cannon3_hide():
        eng.stop_other_scripts(CANNON3)
        CANNON3.hide()
        return
        yield

    @eng.on("go to lobby", CANNON3)
    def _cannon3_lobby():
        eng.stop_other_scripts(CANNON3)
        CANNON3.hide()
        return
        yield

    @eng.on("startshockwaves333", SW3)
    def _sw3_random():
        while True:
            yield from W(R(5, 30))
            eng.broadcast("shock3")

    @eng.on("shock3", SW3)
    def _sw3_move():
        SW3.show()
        SW3.goto(241, -75)
        while SW3.x > -241:
            SW3.x -= 7 / ATK
            yield
        eng.stop_other_scripts(SW3)
        SW3.hide()

    @eng.on("shock3", SW3)
    def _sw3_hit():
        while True:
            if SW3.touching(ANIM):
                eng.broadcast("take damage")
                return
            yield

    @eng.on("enemy 3 oof", SW3)
    def _sw3_off():
        eng.stop_other_scripts(SW3)
        SW3.hide()
        return
        yield

    @eng.on("level3", ABYSS)
    def _abyss():
        ABYSS.back()
        ABYSS.show()
        while True:
            if ABYSS.touching(PLAYER, inset=0.0):
                eng.broadcast("falloofie")
                return
            yield

    # -- Enemy 3 health bar ------------------------------------------------------
    @eng.on("startlv3", E3STAT)
    def _e3stat_in():
        eng.broadcast("restorecanon")
        yield from W(0.05)
        E3STAT.show()
        E3STAT.set_costume("costume3")
        E3STAT.goto(308, -144)
        yield from E3STAT.glide(0.3, 150, -144)

    @eng.on("enemy 3 damage", E3STAT)
    def _e3stat_bounce():
        yield from E3STAT.glide(0.05, 150, -139)
        yield from E3STAT.glide(0.1, 150, -144)

    @eng.on("enemy 3 damage", E3STAT)
    def _e3stat_dmg():
        E3STAT.next_costume()
        yield from W(0.05)
        if E3STAT.costume_is("Oof2"):
            eng.broadcast("enemy 3 oof")

    @eng.on("restoreEN3", E3STAT)
    def _e3stat_restore():
        E3STAT.set_costume("costume3")
        yield from E3STAT.glide(0.05, 150, -139)
        yield from E3STAT.glide(0.1, 150, -144)

    @eng.on("go to lobby", E3STAT)
    def _e3stat_out():
        E3STAT.goto(150, -144)
        yield from E3STAT.glide(0.3, 308, -144)
        E3STAT.hide()

    @eng.on("game over", E3STAT)
    def _e3stat_go():
        E3STAT.hide()
        return
        yield

    # =========================================================================
    # Final level: Riby
    # =========================================================================
    def _riby_run_anim():
        while True:
            for c in ("costume6", "costume8", "costume4", "costume9"):
                RIBY.set_costume(c)
                yield from W(0.01)

    def _riby_laugh(times):
        for _ in range(times):
            RIBY.set_costume("costume16")
            yield from W(0.05)
            RIBY.set_costume("costume15")
            yield from W(0.05)

    def _danger(v):
        V["RibyDanger"] = v
        eng.broadcast("playerfinalenable")

    @eng.on("final cutscene", RIBY)
    def _riby_cutscene():
        RIBY.show()
        RIBY.point(90)
        RIBY.goto(198, -75)
        RIBY.set_costume("door")
        yield from W(0.1)
        RIBY.set_costume("costume2")
        yield from W(1)
        RIBY.set_costume("costume3")
        yield from W(0.05)
        RIBY.set_costume("costume13")
        yield from W(0.7)
        RIBY.set_costume("costume3")
        yield from W(0.05)
        RIBY.set_costume("costume14")
        yield from W(0.05)
        yield from _riby_laugh(10)
        RIBY.point(-90)
        RIBY.set_costume("costume2")
        yield from W(0.2)
        RIBY.set_costume("costume3")
        yield from W(0.05)
        RIBY.set_costume("costume4")
        yield from W(0.05)
        RIBY.goto(198, -75)
        yield from RIBY.glide(0.05, 176, -51)
        eng.broadcast("cutscenehit")
        yield from RIBY.glide(0.05, 143, -38)
        yield from RIBY.glide(0.05, 126, -75)
        RIBY.set_costume("costume11")
        yield from W(0.05)
        RIBY.set_costume("costume2")
        yield from W(1)
        eng.broadcast("startfinallevel")
        eng.broadcast("PlayerEnable")
        eng.broadcast("playerfinalenable")

    @eng.on("startfinallevel", RIBY)
    def _riby_fight_start():
        RIBY.show()
        RIBY.goto(126, -75)
        RIBY.point(-90)
        RIBY.set_costume("costume2")
        yield from W(1)
        eng.broadcast("phase1riby")

    @eng.on("RibyRun", RIBY)
    def _riby_run():
        yield from _riby_run_anim()

    def _riby_cannon_volley(times):
        """Aim the evil cannon at Koki and fire, `times` times."""
        for _ in range(times):
            _danger(1)
            RIBY.set_costume("costume2")
            eng.broadcast("aim canon evil")
            for _ in range(10):
                RIBY.point(90 if ANIM.x > RIBY.x else -90)
                yield from W(0.05)
            eng.broadcast("shootEVILcanonball")
        eng.broadcast("hideEvilCanon")
        _danger(0)
        yield from W(2)
        eng.broadcast("jumpatkriby")

    @eng.on("phase1riby", RIBY)
    def _riby_phase1():
        V["RibyDanger"] = 1
        eng.broadcast("RibyRun")
        eng.broadcast("playerfinalenable")
        RIBY.goto(126, -75)
        yield from RIBY.glide(0.5, 0, -75)
        eng.stop_other_scripts(RIBY)
        eng.broadcast("playerfinalenable")
        RIBY.set_costume("costume10")
        yield from RIBY.glide(0.3, -99, 30)
        yield from RIBY.glide(0.1, -146, -75)
        eng.broadcast("shockwaveriby")
        RIBY.play("groundpound")
        RIBY.set_costume("costume11")
        yield from RIBY.glide(0.1, -151, -75)
        _danger(0)
        yield from _riby_laugh(20)
        _danger(1)
        RIBY.point(90)
        eng.broadcast("RibyRun")
        yield from RIBY.glide(0.5, 126, -75)
        eng.stop_other_scripts(RIBY)
        eng.broadcast("playerfinalenable")
        RIBY.set_costume("costume10")
        RIBY.goto(126, -75)
        yield from RIBY.glide(0.1, 126, 30)
        yield from RIBY.glide(0.1, 126, -75)
        eng.broadcast("shockwaveriby")
        RIBY.play("groundpound")
        RIBY.set_costume("costume11")
        yield from W(0.1)
        _danger(0)
        yield from _riby_laugh(20)
        _danger(1)
        RIBY.point(-90)
        eng.broadcast("RibyRun")
        yield from RIBY.glide(0.5, 0, -75)
        eng.stop_other_scripts(RIBY)
        yield from W(1)
        yield from _riby_cannon_volley(1)

    @eng.on("playerfinalenable", RIBY)
    def _riby_touch():
        while True:
            if RIBY.touching(ANIM):
                if V["RibyDanger"] == 1:
                    eng.broadcast("take damage")
                    return
                if V["RibyDanger"] == 0 and key("x"):
                    eng.broadcast("enemy4 damage")
                    return
            yield

    def _riby_dash_sweeps():
        """Back-and-forth floor dashes, then settle at center."""
        for _ in range(R(2, 6)):
            yield from _riby_laugh(7)
            _danger(1)
            RIBY.point(-90)
            RIBY.play("slide")
            RIBY.set_costume("costume11")
            yield from RIBY.glide(0.2, -200, -75)
            yield from _riby_laugh(7)
            _danger(1)
            RIBY.point(90)
            RIBY.play("slide")
            RIBY.set_costume("costume11")
            yield from RIBY.glide(0.2, 200, -75)
        RIBY.set_costume("costume13")
        yield from W(1)
        _danger(0)
        eng.broadcast("RibyRun")
        RIBY.point(-90)
        yield from RIBY.glide(1, 0, -75)
        eng.stop_other_scripts(RIBY)
        RIBY.set_costume("costume2")
        _danger(0)
        yield from W(1)
        eng.broadcast("jumpatkriby")

    @eng.on("enemy4 damage", RIBY)
    def _riby_damaged():
        V["damageway4"] = R(1, 3)
        eng.stop_other_scripts(RIBY)
        RIBY.play("hit2")
        RIBY.set_costume("OOF")
        yield from _flash(RIBY, 10)
        RIBY.ghost = 0
        _danger(1)
        RIBY.point(-90)
        eng.broadcast("RibyRun")
        yield from RIBY.glide(0.5, 0, -75)
        eng.stop_other_scripts(RIBY)
        _danger(1)
        RIBY.set_costume("costume2")
        yield from W(0.7)
        RIBY.set_costume("costume13")
        yield from W(0.3)
        RIBY.set_costume("costume2")
        yield from W(0.5)
        if V["damageway4"] == 1:
            yield from _riby_cannon_volley(R(1, 3))
        elif V["damageway4"] == 2:
            eng.broadcast("jumpatkriby")
        elif V["damageway4"] == 3:
            eng.broadcast("RibyRun")
            RIBY.point(90)
            yield from RIBY.glide(0.5, 200, -75)
            eng.stop_other_scripts(RIBY)
            _danger(1)
            yield from _riby_dash_sweeps()
        elif V["damageway4"] == 4:
            eng.stop_other_scripts(RIBY)
            _danger(1)
            RIBY.point(90)
            RIBY.set_costume("costume2")
            yield from W(0.3)
            eng.broadcast("RibyRun")
            yield from RIBY.glide(0.6, -56, -75)
            eng.stop_other_scripts(RIBY)
            RIBY.set_costume("costume2")
            yield from W(0.3)
            eng.broadcast("en1final")
            yield from W(1)
            RIBY.set_costume("costume10")
            yield from RIBY.glide(0.1, -1, -4)
            yield from RIBY.glide(0.1, 35, -22)
            yield from RIBY.glide(0.1, 83, -72)
            RIBY.hide()
            eng.stop_other_scripts(RIBY)
            eng.broadcast("possessen1")

    @eng.on("jumpatkriby", RIBY)
    def _riby_jump_attacks():
        for _ in range(R(3, 6)):
            RIBY.set_costume("costume10")
            RIBY.play("jump")
            RIBY.point(90 if RIBY.x < 0 else -90)
            yield from RIBY.glide(0.2, 0, 33)
            RIBY.set_costume("costume17")
            yield from W(0.1)
            RIBY.point(90 if ANIM.x > RIBY.x else -90)
            yield from W(0.2)
            _danger(1)
            yield from RIBY.glide_to_sprite(0.5 * ATK, ANIM)
            RIBY.play("groundpound")
            yield from RIBY.glide(0.01, RIBY.x, -75)
            _danger(0)
            eng.broadcast("shockwaveriby")
            RIBY.set_costume("costume11")
            yield from W(0.05)
            RIBY.set_costume("costume2")
            yield from W(2)
            _danger(1)
        # Every other attack pattern chains into the next by broadcasting;
        # without this the fight soft-locks (Riby inert, player can neither
        # damage him nor die) if the player missed all the pound windows.
        yield from W(1)
        eng.broadcast("jumpatkriby")

    @eng.on("RibyOUT", RIBY)
    def _riby_out():
        RIBY.show()
        RIBY.goto(EN1.x, -75)
        _danger(0)
        yield from _riby_laugh(15)
        _danger(1)
        eng.broadcast("RibyRun")
        RIBY.point(90)
        yield from RIBY.glide(0.5, 200, -75)
        eng.stop_other_scripts(RIBY)
        _danger(1)
        yield from _riby_dash_sweeps()

    @eng.on("enemy 4 oof", RIBY)
    def _riby_dead():
        eng.stop_other_scripts(RIBY)
        RIBY.play("hit")
        RIBY.set_costume("OOF")
        RIBY.clear_fx()
        yield from W(1)
        for _ in range(7):
            RIBY.y += 5
            yield
        yield from RIBY.glide(0.7, RIBY.x, -204)
        RIBY.hide()
        eng.broadcast("door4openagain")

    @eng.on("go to lobby", RIBY)
    def _riby_lobby():
        eng.stop_other_scripts(RIBY)
        RIBY.hide()
        return
        yield

    @eng.on("oofie", RIBY)
    def _riby_playerdead():
        eng.stop_other_scripts(RIBY)
        return
        yield

    # -- Riby's evil cannon + balls ------------------------------------------------
    @eng.on("aim canon evil", EVILC)
    def _evilc_aim():
        EVILC.play("recording1")
        EVILC.front()
        EVILC.clear_fx()
        EVILC.show()
        EVILC.goto_sprite(RIBY)
        EVILC.y = -74
        for _ in range(30):
            EVILC.point(90 if ANIM.x > EVILC.x else -90)
            yield from W(0.05)
        V["evilcanonballdirection"] = 90 if EVILC.direction > 0 else -90

    @eng.on("shootEVILcanonball", EVILC)
    def _evilc_fire():
        EVILC.play("explosion meme")
        yield from _flash(EVILC)
        EVILC.clear_fx()

    @eng.on("hideEvilCanon", EVILC)
    def _evilc_hide():
        eng.stop_other_scripts(EVILC)
        EVILC.hide()
        return
        yield

    @eng.on("go to lobby", EVILC)
    def _evilc_lobby():
        eng.stop_other_scripts(EVILC)
        EVILC.hide()
        return
        yield

    def _mk_evil_ball(spr, step, edge):
        def _fly():
            spr.goto_sprite(EVILC)
            spr.show()
            if step > 0:
                while spr.x < edge:
                    spr.x += step
                    yield
            else:
                while spr.x > edge:
                    spr.x += step
                    yield
            spr.hide()
            eng.stop_other_scripts(spr)
        def _show():
            spr.show()
            spr.front()
            yield from _flash(spr)
        def _hit():
            while True:
                if spr.touching(ANIM):
                    eng.broadcast("take damage")
                    spr.hide()
                    eng.stop_other_scripts(spr)
                    return
                yield
        def _lobby():
            spr.hide()
            return
            yield
        return _fly, _show, _hit, _lobby

    for _spr, _step, _edge in ((CBALL3, 20 / ATK, 240),
                               (CBALL4, -20 / ATK, -240)):
        _fly, _show, _hit, _lobby = _mk_evil_ball(_spr, _step, _edge)
        eng.on("shootEVILcanonball", _spr)(_fly)
        eng.on("shootEVILcanonball", _spr)(_show)
        eng.on("shootEVILcanonball", _spr)(_hit)
        eng.on("go to lobby", _spr)(_lobby)

    # -- Riby's shockwaves (both directions + heal wave) ---------------------------
    @eng.on("shockwaveriby", SW3)
    def _rsw_left():
        SW3.show()
        SW3.goto_sprite(RIBY)
        while SW3.x > -241:
            SW3.x -= 10 / ATK
            yield
        eng.stop_other_scripts(SW3)
        SW3.hide()

    @eng.on("shockwaveriby", SW3)
    def _rsw_left_hit():
        while True:
            if SW3.touching(ANIM):
                eng.broadcast("take damage")
                return
            yield

    @eng.on("shockwaveriby", SW4)
    def _rsw_right():
        SW4.show()
        SW4.goto_sprite(RIBY)
        while SW4.x < 241:
            SW4.x += 10 / ATK
            yield
        eng.stop_other_scripts(SW4)
        SW4.hide()

    @eng.on("shockwaveriby", SW4)
    def _rsw_right_hit():
        while True:
            if SW4.touching(ANIM):
                eng.broadcast("take damage")
                return
            yield

    @eng.on("shockwaveriby", SW5)
    def _rsw_heal():
        yield from W(0.7)
        V["healwavedirection"] = R(1, 2)
        SW5.show()
        SW5.goto_sprite(RIBY)
        if V["healwavedirection"] == 1:
            while SW5.x < 241:
                SW5.x += 10
                yield
        else:
            while SW5.x > -241:
                SW5.x -= 10
                yield
        eng.stop_other_scripts(SW5)
        SW5.hide()

    @eng.on("shockwaveriby", SW5)
    def _rsw_heal_touch():
        while True:
            if SW5.touching(ANIM):
                eng.broadcast("partialrestore")
                SW5.play("pop")
                SW5.hide()
                eng.stop_other_scripts(SW5)
                return
            yield

    # -- Riby health bar -----------------------------------------------------------
    @eng.on("startfinallevel", E4STAT)
    def _e4stat_in():
        eng.broadcast("restorecanon")
        yield from W(0.05)
        E4STAT.show()
        E4STAT.set_costume("costume1")
        E4STAT.goto(308, -144)
        yield from E4STAT.glide(0.3, 150, -144)

    @eng.on("enemy4 damage", E4STAT)
    def _e4stat_bounce():
        yield from E4STAT.glide(0.05, 150, -139)
        yield from E4STAT.glide(0.1, 150, -144)

    @eng.on("enemy4 damage", E4STAT)
    def _e4stat_dmg():
        E4STAT.next_costume()
        yield from W(0.05)
        if E4STAT.costume_name.lower() in ("costume5", "costume6", "costume7"):
            V["damageway4"] = 4
        if E4STAT.costume_is("Oof2"):
            eng.broadcast("enemy 4 oof")

    @eng.on("go to lobby", E4STAT)
    def _e4stat_out():
        E4STAT.goto(150, -144)
        yield from E4STAT.glide(0.3, 308, -144)
        E4STAT.hide()

    @eng.on("game over", E4STAT)
    def _e4stat_go():
        E4STAT.hide()
        return
        yield

    @eng.on("ending cutscene", E4STAT)
    def _e4stat_end():
        eng.stop_other_scripts(E4STAT)
        E4STAT.hide()
        return
        yield

    # =========================================================================
    # Ending: trophy walk + score screen
    # =========================================================================
    @eng.on("ending cutscene", REWARD)
    def _reward():
        REWARD.show()
        REWARD.front()
        while True:
            REWARD.goto_sprite(ANIM)
            yield

    @eng.on("the end", REWARD)
    def _reward_off():
        eng.stop_other_scripts(REWARD)
        REWARD.hide()
        return
        yield

    @eng.on("ending score", SCORE)
    def _score():
        SCORE.front()
        SCORE.show()
        SCORE.set_costume("costume1")
        yield from SCORE.play_until_done("output (33)")
        yield from W(0.6)
        if V["knockouts"] == 0:
            if V["taken damage"] == 0:
                eng.broadcast("a")
            else:
                if V["lives"] > 2:
                    if V["has healed"] > 0:
                        eng.broadcast("c")
                    else:
                        eng.broadcast("d")
                else:
                    eng.broadcast("b")
            return
        if V["lives"] > 2:
            if V["has healed"] > 0:
                eng.broadcast("c")
            else:
                eng.broadcast("d")
            return
        if V["knockouts"] in (1, 2):
            eng.broadcast("c")
            return
        if V["knockouts"] > 6:
            eng.broadcast("f")
            return
        eng.broadcast("b")

    _grades = {
        "a": ("costume5", "Koki A score music"),
        "b": ("costume7", "lowatrezzo"),
        "c": ("costume9", "Koki C Ending score music"),
        "d": ("costume10", "Koki D score"),
        "f": ("costume11", "Koki F score"),
    }

    def _mk_grade(costume, track):
        def _show():
            SCORE.set_costume(costume)
            SCORE.music(track)
            return
            yield
        def _input():
            yield from W(1)
            while True:
                if key("enter"):
                    eng.stop_other_scripts(SCORE)
                    eng.sound.stop_all()
                    eng.broadcast("whitechange")
                    yield from W(0.05)
                    eng.broadcast("go to lobby")
                    SCORE.hide()
                    return
                yield
        return _show, _input

    for _g, (_c, _t) in _grades.items():
        _show, _input = _mk_grade(_c, _t)
        eng.on(_g, SCORE)(_show)
        eng.on(_g, SCORE)(_input)
