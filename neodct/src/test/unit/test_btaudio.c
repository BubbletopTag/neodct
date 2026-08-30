/* test_btaudio.c -- the routing, which is the whole feature in one file.
 *
 * These are string tests, and that is the point: what ALSA does is decided by
 * the bytes in /run/asound.conf, so getting those bytes right IS getting the
 * routing right. A test that mocked ALSA would prove nothing about the phone.
 */

#include <stdio.h>
#include <string.h>

#include "nd_btaudio.h"

#include "platform_test.h"

/* With no device, "default" is the USB card -- and it has to be BYTE-IDENTICAL
 * in shape to what S17audio writes at boot, because that is what disconnecting
 * has to restore. "type plug" over "type hw", with dmix nowhere in it. */
static void test_speaker_route_matches_the_boot_script(void)
{
    char text[ND_BTAUDIO_CONF_MAX];

    CHECK_INT(nd_btaudio_asound_text(text, sizeof text, NULL, 1), ND_OK);

    CHECK(strstr(text, "pcm.!default") != NULL);
    CHECK(strstr(text, "type plug") != NULL);
    CHECK(strstr(text, "type hw") != NULL);
    CHECK(strstr(text, "card 1") != NULL);
    /* never dmix: this kernel has no ALSA timer and opening it fails */
    CHECK(strstr(text, "dmix") == NULL);
    CHECK(strstr(text, "bluealsa") == NULL);
}

/* With a device connected, "default" becomes that device's A2DP sink. The
 * address is what bluealsa keys on, so it has to reach the config verbatim --
 * a truncated or reformatted address is a PCM that does not exist, and ALSA
 * reports that as a failed open rather than as silence. */
static void test_earbud_route_names_the_device(void)
{
    char text[ND_BTAUDIO_CONF_MAX];

    CHECK_INT(nd_btaudio_asound_text(text, sizeof text, "AA:BB:CC:DD:EE:FF", 1), ND_OK);

    CHECK(strstr(text, "pcm.!default") != NULL);
    CHECK(strstr(text, "bluealsa") != NULL);
    CHECK(strstr(text, "AA:BB:CC:DD:EE:FF") != NULL);
    CHECK(strstr(text, "a2dp") != NULL);
    /* and NOT the speaker: the point is that the card stops being the default */
    CHECK(strstr(text, "type hw") == NULL);
}

/* The earbuds need the SAME plug wrapper the speaker has, and this is the
 * check that was missing when the route shipped without one.
 *
 * A bare bluealsa PCM offers only what A2DP negotiated -- 44100 or 48000,
 * stereo -- while every noise on this phone is aplay asking for whatever suits
 * it: Koki's mixer is 22050 mono, MusicPlayer passes the file's own rate and
 * channels, nd_notify hardcodes 44100 stereo. Without plug, hw_params refuses
 * anything that is not already A2DP's format and the app plays silence.
 *
 * It hid because 44100 stereo IS what A2DP wants, so the ringtone always
 * matched and an ordinary music file did too. Koki's 22050 mono never could,
 * which is why the report was "the game has no Bluetooth sound but music
 * does". */
static void test_earbud_route_converts_rate_and_channels(void)
{
    char text[ND_BTAUDIO_CONF_MAX];
    const char *plug;
    const char *bluealsa;

    CHECK_INT(nd_btaudio_asound_text(text, sizeof text, "AA:BB:CC:DD:EE:FF", 1), ND_OK);

    plug = strstr(text, "type plug");
    bluealsa = strstr(text, "type bluealsa");
    CHECK(plug != NULL);
    CHECK(bluealsa != NULL);

    /* Order is the whole point: plug has to be the OUTER pcm with bluealsa as
     * its slave. The two names both being present would also be true of a
     * config that wrapped them the wrong way round, which converts nothing. */
    CHECK(plug < bluealsa);
    CHECK(strstr(text, "slave.pcm") != NULL);
}

/* The ordinary line. The name keeps its spaces: "Skullcandy Indy ANC" is one
 * name, not three fields, and splitting on whitespace would show "Skullcandy"
 * in the list and lose the rest. */
static void test_parse_one_device(void)
{
    static const char TEXT[] = "Device AA:BB:CC:DD:EE:FF Skullcandy Indy ANC\n";
    nd_btaudio_device found[4];
    size_t n;

    n = nd_btaudio_parse_devices(TEXT, found, 4u);

    CHECK_INT(n, 1);
    CHECK_STR(found[0].addr, "AA:BB:CC:DD:EE:FF");
    CHECK_STR(found[0].name, "Skullcandy Indy ANC");
}

/* Real output has other lines in it, and more than one device. Anything that
 * is not a device line is skipped rather than counted or refused. */
static void test_parse_skips_what_is_not_a_device(void)
{
    static const char TEXT[] = "Agent registered\n"
                               "Device AA:BB:CC:DD:EE:FF Skullcandy Indy ANC\n"
                               "Changing power on succeeded\n"
                               "Device 11:22:33:44:55:66 Kitchen Speaker\n";
    nd_btaudio_device found[4];
    size_t n;

    n = nd_btaudio_parse_devices(TEXT, found, 4u);

    CHECK_INT(n, 2);
    CHECK_STR(found[0].name, "Skullcandy Indy ANC");
    CHECK_STR(found[1].addr, "11:22:33:44:55:66");
    CHECK_STR(found[1].name, "Kitchen Speaker");
}

/* While a scan is running bluetoothctl prefixes what it finds with [NEW], and
 * that is exactly the output the Scan screen reads. A parser anchored to the
 * start of the line would find nothing at the moment it matters most. */
static void test_parse_reads_scan_output_with_its_markers(void)
{
    static const char TEXT[] = "[NEW] Device AA:BB:CC:DD:EE:FF Skullcandy Indy ANC\r\n";
    nd_btaudio_device found[2];
    size_t n;

    n = nd_btaudio_parse_devices(TEXT, found, 2u);

    CHECK_INT(n, 1);
    CHECK_STR(found[0].addr, "AA:BB:CC:DD:EE:FF");
    /* the \r is the terminal's, not part of the name */
    CHECK_STR(found[0].name, "Skullcandy Indy ANC");
}

/* A device that has not told us its name: bluetoothctl repeats the address
 * with dashes. Keep it. A blank row is worse than an ugly one -- the owner can
 * still tell two of them apart, and can still pick one. */
static void test_parse_keeps_a_nameless_device(void)
{
    static const char TEXT[] = "Device AA:BB:CC:DD:EE:FF AA-BB-CC-DD-EE-FF\n";
    nd_btaudio_device found[2];
    size_t n;

    n = nd_btaudio_parse_devices(TEXT, found, 2u);

    CHECK_INT(n, 1);
    CHECK_STR(found[0].name, "AA-BB-CC-DD-EE-FF");
}

/* More devices than there is room for stops at the room there is. A flat full
 * of Bluetooth must not walk off the end of the caller's array. */
static void test_parse_stops_at_max(void)
{
    static const char TEXT[] = "Device 11:11:11:11:11:11 One\n"
                               "Device 22:22:22:22:22:22 Two\n"
                               "Device 33:33:33:33:33:33 Three\n";
    nd_btaudio_device found[2];
    size_t n;

    n = nd_btaudio_parse_devices(TEXT, found, 2u);

    CHECK_INT(n, 2);
    CHECK_STR(found[1].name, "Two");
}

/* Earbuds in the case: paired, trusted, and not connected. Matching "yes"
 * anywhere would route music into a closed box, which is the single most
 * likely way for this feature to look broken. */
static void test_paired_but_not_connected_is_not_connected(void)
{
    static const char TEXT[] = "Device AA:BB:CC:DD:EE:FF (public)\n"
                               "\tName: Skullcandy Indy ANC\n"
                               "\tPaired: yes\n"
                               "\tTrusted: yes\n"
                               "\tBlocked: no\n"
                               "\tConnected: no\n";

    CHECK(nd_btaudio_parse_connected(TEXT) == false);
}

/* And the affirmative, which is what actually drives the code -- the refusal
 * above passes against a stub that always says no. */
static void test_connected_yes(void)
{
    static const char TEXT[] = "Device AA:BB:CC:DD:EE:FF (public)\n"
                               "\tPaired: yes\n"
                               "\tConnected: yes\n"
                               "\tUUID: Audio Sink\n";

    CHECK(nd_btaudio_parse_connected(TEXT) == true);
}

/* A scan is the awkward one: "scan on" never returns by itself, so the timeout
 * is what makes it a scan of a known length rather than a hang. */
static void test_scan_command_carries_its_timeout(void)
{
    nd_btaudio_cmd cmd;
    size_t i = 0u;

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "scan", "on", 8), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "scan", "on", 8) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_BTCTL);
    CHECK_STR(cmd.argv[i++], "--timeout");
    CHECK_STR(cmd.argv[i++], "8");
    CHECK_STR(cmd.argv[i++], "scan");
    CHECK_STR(cmd.argv[i++], "on");
    CHECK(cmd.argv[i] == NULL);
}

/* Without a timeout the flag is absent entirely, not passed as 0 -- which
 * bluetoothctl reads as "no timeout" and would hang. */
static void test_info_command_has_no_timeout(void)
{
    nd_btaudio_cmd cmd;
    size_t i = 0u;

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "info", "AA:BB:CC:DD:EE:FF", 0), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "info", "AA:BB:CC:DD:EE:FF", 0) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_BTCTL);
    CHECK_STR(cmd.argv[i++], "info");
    CHECK_STR(cmd.argv[i++], "AA:BB:CC:DD:EE:FF");
    CHECK(cmd.argv[i] == NULL);
}

/* Pairing needs an agent, and this is the bug that hid behind every other
 * Bluetooth bug in this file. BlueZ will not complete Secure Simple Pairing
 * unless SOME agent is registered to answer for the local side; with none it
 * cancels the exchange. bluetoothctl only registers one when asked, and a
 * one-shot `bluetoothctl pair XX` does not ask.
 *
 * What that looks like is nothing like "no agent": the phone says "Pairing..."
 * and then "Could not connect", and the earbuds announce "ready to pair"
 * a second time because from their side the attempt was simply abandoned.
 * The record BlueZ leaves behind is the giveaway -- Name, Class and a full
 * Services list, so a connection plainly happened, but no [LinkKey] section,
 * so it was never authenticated.
 *
 * NoInputNoOutput is the honest capability for a phone with no pairing UI, and
 * it is also the useful one: it selects the "Just Works" association model,
 * which BlueZ auto-confirms for a pairing WE initiated. The agent has to exist;
 * it never has to be asked anything. */
static void test_pair_registers_an_agent(void)
{
    nd_btaudio_cmd cmd;
    size_t i = 0u;

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "pair", "AA:BB:CC:DD:EE:FF", 0), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "pair", "AA:BB:CC:DD:EE:FF", 0) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_BTCTL);
    CHECK_STR(cmd.argv[i++], "--agent");
    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_AGENT);
    CHECK_STR(cmd.argv[i++], "pair");
    CHECK_STR(cmd.argv[i++], "AA:BB:CC:DD:EE:FF");
    CHECK(cmd.argv[i] == NULL);
}

/* connect gets one too: connecting a device that is not bonded starts a
 * pairing, so it can land in the same hole. */
static void test_connect_registers_an_agent(void)
{
    nd_btaudio_cmd cmd;
    size_t i = 0u;

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "connect", "AA:BB:CC:DD:EE:FF", 0), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "connect", "AA:BB:CC:DD:EE:FF", 0) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_BTCTL);
    CHECK_STR(cmd.argv[i++], "--agent");
    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_AGENT);
    CHECK_STR(cmd.argv[i++], "connect");
}

/* The verbs that only ask questions do not register anything. An agent
 * registration is a D-Bus round trip and a default-agent claim; taking it out
 * on every `devices` poll would be noise, and claiming the default agent while
 * a pairing is in flight is worse than noise. */
static void test_query_verbs_register_no_agent(void)
{
    nd_btaudio_cmd cmd;

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "devices", NULL, 0), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "devices", NULL, 0) != ND_OK)
        return;
    CHECK_STR(cmd.argv[1], "devices");

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "scan", "on", 8), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "scan", "on", 8) != ND_OK)
        return;
    CHECK_STR(cmd.argv[1], "--timeout");
}

/* Everything at once still fits argv with room for the NULL. */
static void test_agent_and_timeout_together_fit(void)
{
    nd_btaudio_cmd cmd;
    size_t i = 0u;

    CHECK_INT(nd_btaudio_cmd_build(&cmd, "pair", "AA:BB:CC:DD:EE:FF", 5), ND_OK);
    if (nd_btaudio_cmd_build(&cmd, "pair", "AA:BB:CC:DD:EE:FF", 5) != ND_OK)
        return;

    while (i < ND_BTAUDIO_ARGV_MAX && cmd.argv[i] != NULL)
        i++;
    CHECK(i < ND_BTAUDIO_ARGV_MAX);
    CHECK(cmd.argv[i] == NULL);
}

/* bluealsa's -p names the profile the LOCAL device implements, not the one the
 * peer has. Earbuds are an A2DP Sink; for the phone to send them music it must
 * register as an A2DP SOURCE. Registering a2dp-sink offers the phone as a
 * speaker instead, so a connection to earbuds finds no complementary profile
 * on either side and BlueZ refuses it:
 *
 *   Failed to connect: org.bluez.Error.Failed br-connection-profile-unavailable
 *
 * which says nothing about profiles being backwards and is easy to read as a
 * pairing problem, because it appears at exactly the moment pairing would. */
static void test_bluealsa_registers_a2dp_source(void)
{
    nd_btaudio_cmd cmd;
    size_t i = 0u;

    CHECK_INT(nd_btaudio_bluealsa_cmd(&cmd), ND_OK);
    if (nd_btaudio_bluealsa_cmd(&cmd) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], ND_BTAUDIO_BLUEALSA);
    CHECK_STR(cmd.argv[i++], "-p");
    CHECK_STR(cmd.argv[i++], "a2dp-source");
    CHECK(cmd.argv[i] == NULL);
}

/* Going to the earbuds keeps the speaker route rather than remembering how to
 * rebuild it. What S17audio wrote at boot is the truth about which card this
 * phone plays through; deriving it a second time later is a chance to derive
 * it differently. */
static void test_route_to_earbuds_saves_the_speaker_route(void)
{
    char back[ND_BTAUDIO_CONF_MAX];
    static const char BOOT[] = "# Generated by /etc/init.d/S17audio.\n"
                               "pcm.!default { type plug slave.pcm { type hw card 3 } }\n";

    pt_mkdir("/run");
    pt_write_text(ND_BTAUDIO_ASOUND, BOOT);

    CHECK_INT(nd_btaudio_route_to("AA:BB:CC:DD:EE:FF", 3), ND_OK);

    /* the live config is now the earbuds */
    CHECK(pt_read_text(ND_BTAUDIO_ASOUND, back, sizeof back) != (size_t)-1);
    CHECK(strstr(back, "bluealsa") != NULL);
    CHECK(strstr(back, "AA:BB:CC:DD:EE:FF") != NULL);

    /* and what was there is kept, exactly */
    CHECK(pt_read_text(ND_BTAUDIO_ASOUND_SAVED, back, sizeof back) != (size_t)-1);
    CHECK_STR(back, BOOT);
}

/* Coming back restores those bytes rather than writing new ones. */
static void test_route_back_restores_what_was_saved(void)
{
    char back[ND_BTAUDIO_CONF_MAX];
    static const char BOOT[] = "# Generated by /etc/init.d/S17audio.\n"
                               "pcm.!default { type plug slave.pcm { type hw card 3 } }\n";

    pt_mkdir("/run");
    pt_write_text(ND_BTAUDIO_ASOUND, BOOT);

    CHECK_INT(nd_btaudio_route_to("AA:BB:CC:DD:EE:FF", 3), ND_OK);
    CHECK_INT(nd_btaudio_route_to(NULL, 3), ND_OK);

    CHECK(pt_read_text(ND_BTAUDIO_ASOUND, back, sizeof back) != (size_t)-1);
    CHECK_STR(back, BOOT);
    /* and the saved copy is gone, so a later restore cannot resurrect it */
    CHECK(!nd_path_exists(ND_BTAUDIO_ASOUND_SAVED));
}

/* Nothing saved -- the phone rebooted with earbuds configured, say. Write a
 * speaker route rather than leaving audio pointed at something absent. */
static void test_route_back_with_nothing_saved_writes_the_speaker(void)
{
    char back[ND_BTAUDIO_CONF_MAX];

    pt_mkdir("/run");

    CHECK_INT(nd_btaudio_route_to(NULL, 2), ND_OK);

    CHECK(pt_read_text(ND_BTAUDIO_ASOUND, back, sizeof back) != (size_t)-1);
    CHECK(strstr(back, "card 2") != NULL);
    CHECK(strstr(back, "bluealsa") == NULL);
}

int main(void)
{
    RUN(test_speaker_route_matches_the_boot_script);
    RUN(test_earbud_route_names_the_device);
    RUN(test_earbud_route_converts_rate_and_channels);
    RUN(test_parse_one_device);
    RUN(test_parse_skips_what_is_not_a_device);
    RUN(test_parse_reads_scan_output_with_its_markers);
    RUN(test_parse_keeps_a_nameless_device);
    RUN(test_parse_stops_at_max);
    RUN(test_paired_but_not_connected_is_not_connected);
    RUN(test_connected_yes);
    RUN(test_scan_command_carries_its_timeout);
    RUN(test_info_command_has_no_timeout);
    RUN(test_pair_registers_an_agent);
    RUN(test_connect_registers_an_agent);
    RUN(test_query_verbs_register_no_agent);
    RUN(test_agent_and_timeout_together_fit);
    RUN(test_bluealsa_registers_a2dp_source);
    RUN(test_route_to_earbuds_saves_the_speaker_route);
    RUN(test_route_back_restores_what_was_saved);
    RUN(test_route_back_with_nothing_saved_writes_the_speaker);
    return pt_report("test_btaudio");
}
