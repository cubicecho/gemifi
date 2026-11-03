// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Event;
import gemifi.bitwig.core.Protocol.Frame;
import gemifi.bitwig.core.Protocol.Hello;
import gemifi.bitwig.core.Protocol.ProtocolException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/** The codec against protocol/test-vectors.json (generated from the reference codec in tools/host). */
class ProtocolTest {
    static JsonObject vectors;

    @BeforeAll
    static void load() throws IOException {
        String prop = System.getProperty("gemifi.testVectors");
        Path p = prop != null ? Paths.get(prop) : Paths.get("../../protocol/test-vectors.json");
        vectors = JsonParser.parseString(new String(Files.readAllBytes(p), StandardCharsets.UTF_8)).getAsJsonObject();
    }

    static byte[] hex(String s) {
        return Protocol.fromHex(s);
    }

    static JsonObject frameByDesc(JsonArray frames, String descStart) {
        for (JsonElement e : frames) {
            JsonObject o = e.getAsJsonObject();
            if (o.get("desc").getAsString().startsWith(descStart)) return o;
        }
        throw new AssertionError("no vector starting with " + descStart);
    }

    @Test
    void integerEncodings() throws ProtocolException {
        for (JsonElement e : vectors.getAsJsonArray("u14")) {
            JsonObject o = e.getAsJsonObject();
            int v = o.get("value").getAsInt();
            assertArrayEquals(hex(o.get("bytes").getAsString()), Protocol.u14(v));
            assertEquals(v, Protocol.decU14(hex(o.get("bytes").getAsString()), 0));
        }
        for (JsonElement e : vectors.getAsJsonArray("u28")) {
            JsonObject o = e.getAsJsonObject();
            long v = o.get("value").getAsLong();
            assertArrayEquals(hex(o.get("bytes").getAsString()), Protocol.u28(v));
            assertEquals(v, Protocol.decU28(hex(o.get("bytes").getAsString()), 0));
        }
        for (JsonElement e : vectors.getAsJsonArray("u32")) {
            JsonObject o = e.getAsJsonObject();
            long v = o.get("value").getAsLong();
            assertArrayEquals(hex(o.get("bytes").getAsString()), Protocol.u32(v));
            assertEquals(v, Protocol.decU32(hex(o.get("bytes").getAsString()), 0));
        }
        for (JsonElement e : vectors.getAsJsonObject("config").getAsJsonArray("s32")) {
            JsonObject o = e.getAsJsonObject();
            int v = o.get("value").getAsInt();
            assertArrayEquals(hex(o.get("bytes").getAsString()), Protocol.s32(v));
            assertEquals(v, Protocol.decS32(hex(o.get("bytes").getAsString()), 0));
        }
    }

    @Test
    void s14() {
        assertEquals(-1, Protocol.s14ToInt(0x3FFF));
        assertEquals(-8192, Protocol.s14ToInt(0x2000));
        assertEquals(8191, Protocol.s14ToInt(0x1FFF));
        assertEquals(3, Protocol.s14ToInt(3));
        assertEquals(0x3FFD, Protocol.intToS14(-3));
        assertThrows(IllegalArgumentException.class, () -> Protocol.intToS14(8192));
    }

    @Test
    void pack7() throws ProtocolException {
        for (JsonElement e : vectors.getAsJsonArray("pack7")) {
            JsonObject o = e.getAsJsonObject();
            byte[] raw = hex(o.get("raw").getAsString());
            byte[] packed = hex(o.get("packed").getAsString());
            assertArrayEquals(packed, Protocol.pack7(raw));
            assertArrayEquals(raw, Protocol.unpack7(packed));
        }
    }

    @Test
    void checksum() {
        for (JsonElement e : vectors.getAsJsonArray("checksum")) {
            JsonObject o = e.getAsJsonObject();
            byte[] body = hex(o.get("body").getAsString());
            assertEquals(o.get("chk").getAsInt(), Protocol.checksum(body, 0, body.length));
        }
    }

    @Test
    void framesRoundTrip() throws ProtocolException {
        int n = 0;
        for (String section : new String[] {"frames", "feedback"}) {
            JsonArray frames = section.equals("frames") ? vectors.getAsJsonArray("frames")
                    : vectors.getAsJsonObject("feedback").getAsJsonArray("frames");
            for (JsonElement e : frames) {
                JsonObject o = e.getAsJsonObject();
                byte[] payload = hex(o.get("payload").getAsString());
                byte[] frame = hex(o.get("frame").getAsString());
                int cmd = o.get("cmd").getAsInt(), seq = o.get("seq").getAsInt();
                assertArrayEquals(frame, Protocol.buildFrame(cmd, seq, payload), o.get("desc").getAsString());
                Frame f = Protocol.parseFrame(frame);
                assertEquals(cmd, f.cmd);
                assertEquals(seq, f.seq);
                assertArrayEquals(payload, f.payload);
                n++;
            }
        }
        assertTrue(n > 20);
    }

    @Test
    void badFrames() {
        for (JsonElement e : vectors.getAsJsonArray("bad_frames")) {
            JsonObject o = e.getAsJsonObject();
            ProtocolException ex = assertThrows(ProtocolException.class,
                    () -> Protocol.parseFrame(hex(o.get("frame").getAsString())));
            assertTrue(ex.getMessage().contains(o.get("error").getAsString()), ex.getMessage());
        }
    }

    @Test
    void helloRequestAndReplies() throws ProtocolException {
        JsonArray frames = vectors.getAsJsonArray("frames");
        assertArrayEquals(hex(frameByDesc(frames, "HELLO request").get("frame").getAsString()),
                Protocol.buildFrame(Protocol.HELLO, 0, new byte[0]));

        Frame f = Protocol.parseFrame(hex(frameByDesc(frames, "HELLO reply from an app").get("frame").getAsString()));
        Hello h = Protocol.parseHello(f.payload);
        assertEquals(0, h.role);
        assertEquals(1, h.boardId);
        assertEquals("0.1.5", h.versionText());
        assertEquals(Protocol.CAP_CONFIG | Protocol.CAP_LOG | Protocol.CAP_EVENT | Protocol.CAP_PRESETS, h.caps);
        assertFalse(h.hasFeedback());
        assertEquals("keystation-mini32", h.node);
        assertArrayEquals(hex("10 32 54 76 98 BA DC FE 01 23 45 67"), h.uid);
        assertArrayEquals(f.payload, Protocol.buildHelloReply(h));

        Hello boot = Protocol.parseHello(Protocol.parseFrame(
                hex(frameByDesc(frames, "HELLO reply from bootloader").get("frame").getAsString())).payload);
        assertEquals(1, boot.role);
        assertNull(boot.node);
        assertEquals(256, boot.maxChunk);
    }

    @Test
    void helloNodeRules() throws ProtocolException {
        Hello h = new Hello(0, 3, 0x020000, 1, 0, Protocol.CAP_FEEDBACK, new byte[12], "Bad_Name");
        assertNull(Protocol.parseHello(Protocol.buildHelloReply(h)).node);
        byte[] base = Protocol.buildHelloReply(new Hello(0, 3, 0, 1, 0, 0, new byte[12], null));
        byte[] runsPast = Protocol.concat(base, Protocol.bytes(5, 'a', 'b'));
        assertNull(Protocol.parseHello(runsPast).node);
        byte[] extra = Protocol.concat(base, Protocol.bytes(2, 'a', 'b', 0x11, 0x22));
        assertEquals("ab", Protocol.parseHello(extra).node);
        assertThrows(ProtocolException.class, () -> Protocol.parseHello(new byte[10]));
    }

    @Test
    void event() throws ProtocolException {
        JsonObject o = frameByDesc(vectors.getAsJsonArray("frames"), "EVENT key");
        Frame f = Protocol.parseFrame(hex(o.get("frame").getAsString()));
        assertEquals(Protocol.EVENT, f.cmd);
        Event ev = Protocol.parseEvent(f.payload);
        assertEquals(2, ev.type);
        assertEquals(12, ev.index);
        assertEquals(100, ev.value);
        assertEquals(70000, ev.tMs);
        assertArrayEquals(f.payload, Protocol.buildEvent(2, 12, 100, 70000));
        assertEquals(-2, Protocol.parseEvent(Protocol.buildEvent(4, 0, -2, 0)).delta());
    }

    /** A JSON field that may be null (an added field older firmware doesn't send). */
    static Integer optInt(JsonObject o, String name) {
        JsonElement e = o.get(name);
        return e == null || e.isJsonNull() ? null : e.getAsInt();
    }

    @Test
    void getComponent() throws ProtocolException {
        JsonObject fb = vectors.getAsJsonObject("feedback");
        assertEquals(Protocol.CAP_FEEDBACK, fb.get("cap_feedback").getAsInt());
        assertEquals(Protocol.NONE_INDEX, fb.get("none_index").getAsInt());
        int replies = 0;
        Set<String> seen = new HashSet<>();
        for (JsonElement e : fb.getAsJsonArray("frames")) {
            JsonObject o = e.getAsJsonObject();
            JsonObject fields = o.getAsJsonObject("fields");
            byte[] payload = hex(o.get("payload").getAsString());
            int cmd = o.get("cmd").getAsInt();
            if (cmd == Protocol.GET_COMPONENT) {
                assertArrayEquals(payload, Protocol.reqGetComponent(fields.get("kind").getAsInt(), fields.get("index").getAsInt()));
            } else if (cmd == (Protocol.GET_COMPONENT | Protocol.REPLY)) {
                if (fields.get("status").getAsInt() != Protocol.OK) {
                    assertEquals(Protocol.BAD_PARAM, payload[0]);
                    assertThrows(ProtocolException.class, () -> Protocol.parseComponentReply(payload));
                    continue;
                }
                Component c = Protocol.parseComponentReply(payload);
                assertEquals(fields.get("kind").getAsInt(), c.kind);
                assertEquals(fields.get("index").getAsInt(), c.index);
                assertEquals(fields.get("count").getAsInt(), c.count);
                assertEquals(fields.get("id").getAsString(), c.id);
                assertEquals(fields.get("name").getAsString(), c.name);
                assertEquals(fields.get("role").getAsString(), c.role);
                switch (c.kind) {
                    case Protocol.KIND_BUTTON:
                        assertEquals(fields.get("led").isJsonNull() ? Protocol.NONE_INDEX : fields.get("led").getAsInt(), c.led);
                        break;
                    case Protocol.KIND_KEYS:
                        assertEquals(fields.get("keys").getAsInt(), c.keys);
                        assertEquals(fields.get("first_note").getAsInt(), c.firstNote);
                        break;
                    case Protocol.KIND_ENCODER:
                        assertEquals(fields.get("ring").getAsInt(), c.ring);
                        Integer center = optInt(fields, "center");
                        assertEquals(center == null ? Protocol.NONE_INDEX : center, c.center);
                        break;
                    case Protocol.KIND_LED:
                        assertEquals(fields.get("colours").getAsInt(), c.colours);
                        break;
                    case Protocol.KIND_DISPLAY:
                        assertEquals(fields.get("cols").getAsInt(), c.cols);
                        assertEquals(fields.get("rows").getAsInt(), c.rows);
                        assertEquals(fields.get("type").getAsInt(), c.displayType);
                        break;
                    default:
                        break;
                }
                // sysex.md §4.6.1 "Added fields": present, or null when older firmware leaves them out
                String desc = o.get("desc").getAsString();
                if (fields.has("takeover")) {
                    assertEquals(optInt(fields, "takeover"), c.takeover, desc);
                    seen.add("takeover " + (c.takeover == null));
                }
                if (fields.has("mode")) {
                    assertEquals(optInt(fields, "mode"), c.mode, desc);
                    seen.add("mode " + (c.mode == null));
                }
                if (fields.has("owner")) {
                    assertEquals(optInt(fields, "owner"), c.owner, desc);
                    seen.add("owner " + (c.owner == null));
                }
                assertArrayEquals(payload, Protocol.buildComponentReply(c), desc);
                replies++;
            }
        }
        assertEquals(15, replies);
        assertEquals(Set.of("takeover false", "takeover true", "mode false", "mode true",
                "owner false", "owner true"), seen);
    }

    @Test
    void addedComponentFields() throws ProtocolException {
        Component a = new Component(Protocol.KIND_ANALOG, 0, 1, "", "", "");
        a.takeover = Protocol.TAKEOVER_PICKUP;
        assertArrayEquals(hex("00 03 00 00 01 00 00 00 00 01"), Protocol.buildComponentReply(a));
        Component e = new Component(Protocol.KIND_ENCODER, 0, 1, "", "", "");
        e.ring = 5;
        e.center = 200;
        e.mode = Protocol.ENC_REL_OFFSET;
        byte[] eb = Protocol.buildComponentReply(e);
        assertArrayEquals(hex("05 48 01 02"), Arrays.copyOfRange(eb, eb.length - 4, eb.length));
        Component x = new Component(Protocol.KIND_LED, 0, 1, "", "", "");
        x.colours = 2;
        x.owner = Protocol.OWNER_LOCAL;
        byte[] xb = Protocol.buildComponentReply(x);
        assertArrayEquals(hex("02 02"), Arrays.copyOfRange(xb, xb.length - 2, xb.length));
        for (Component c : new Component[] {a, e, x}) {
            Component got = Protocol.parseComponentReply(Protocol.buildComponentReply(c));
            assertEquals(c.takeover, got.takeover);
            assertEquals(c.mode, got.mode);
            assertEquals(c.owner, got.owner);
            assertTrue(got.reportsAddedFields());
        }
        Component oldEnc = new Component(Protocol.KIND_ENCODER, 0, 1, "", "", "");
        oldEnc.ring = 3;
        Component old = Protocol.parseComponentReply(Protocol.buildComponentReply(oldEnc));
        assertNull(old.mode);
        assertTrue(old.takesValue());
        assertFalse(old.reportsAddedFields());

        Component none = new Component(Protocol.KIND_ANALOG, 0, 1, "", "", "");
        assertTrue(none.takesValue());                   // not reported: as before
        none.takeover = Protocol.TAKEOVER_NONE;
        assertFalse(none.takesValue());
        assertTrue(a.takesValue());
        Component mcu = new Component(Protocol.KIND_ENCODER, 0, 1, "", "", "");
        mcu.mode = Protocol.ENC_MCU;
        assertFalse(mcu.takesValue());
        Component abs = new Component(Protocol.KIND_ENCODER, 0, 1, "", "", "");
        abs.mode = Protocol.ENC_ABSOLUTE;
        assertTrue(abs.takesValue());
        assertTrue(x.localOnly());
        assertFalse(new Component(Protocol.KIND_LED, 0, 1, "", "", "").localOnly());
        Component host = new Component(Protocol.KIND_LED, 0, 1, "", "", "");
        host.owner = Protocol.OWNER_HOST_THEN_LOCAL;
        assertFalse(host.localOnly());
    }

    @Test
    void booted() throws ProtocolException {
        int n = 0;
        for (JsonElement e : vectors.getAsJsonObject("feedback").getAsJsonArray("frames")) {
            JsonObject o = e.getAsJsonObject();
            if (o.get("cmd").getAsInt() != Protocol.BOOTED) continue;
            JsonObject f = o.getAsJsonObject("fields");
            Frame fr = Protocol.parseFrame(hex(o.get("frame").getAsString()));
            assertTrue(fr.isUnsolicited());
            Hello h = Protocol.parseBooted(fr.payload);
            assertEquals(f.get("role").getAsInt(), h.role);
            assertEquals(f.get("board_id").getAsInt(), h.boardId);
            assertEquals(f.get("fw_version").getAsLong(), h.fwVersion);
            assertEquals(f.get("proto_version").getAsInt(), h.protoVersion);
            assertEquals(f.get("max_chunk").getAsInt(), h.maxChunk);
            assertEquals(f.get("caps").getAsInt(), h.caps);
            assertArrayEquals(hex(f.get("uid").getAsString()), h.uid);
            assertEquals(f.get("node").getAsString(), h.node);
            assertTrue(h.hasFeedback());
            byte[] reply = Protocol.buildHelloReply(h);
            assertArrayEquals(fr.payload, Arrays.copyOfRange(reply, 1, reply.length));
            n++;
        }
        assertEquals(1, n);
    }

    @Test
    void componentReplyErrors() {
        Component c = new Component(Protocol.KIND_ENCODER, 0, 1, "e", "E", "pan_1");
        byte[] full = Protocol.buildComponentReply(c);
        byte[] cut = java.util.Arrays.copyOf(full, full.length - 1);
        assertThrows(ProtocolException.class, () -> Protocol.parseComponentReply(cut));
        byte[] badLen = full.clone();
        badLen[6] = 40;                                   // id lstring length > 31
        assertThrows(ProtocolException.class, () -> Protocol.parseComponentReply(badLen));
    }

    @Test
    void lstringCutsAt31AndReplacesNonPrintable() throws ProtocolException {
        byte[] enc = Protocol.encLstring("0123456789012345678901234567890123456789");
        assertEquals(32, enc.length);
        assertEquals(31, enc[0]);
        assertEquals("a?b", Protocol.decLstring(Protocol.bytes(3, 'a', 0x01, 'b'), new int[] {0}));
    }

    @Test
    void feedbackNotifications() {
        JsonArray frames = vectors.getAsJsonObject("feedback").getAsJsonArray("frames");
        int n = 0;
        for (JsonElement e : frames) {
            JsonObject o = e.getAsJsonObject();
            int cmd = o.get("cmd").getAsInt();
            if (cmd < Protocol.NOTIFY_FIRST || cmd > Protocol.NOTIFY_LAST) continue;
            JsonObject f = o.getAsJsonObject("fields");
            byte[] payload;
            switch (cmd) {
                case Protocol.FB_LED:
                    payload = Protocol.fbLed(f.get("led").getAsInt(), f.get("level").getAsInt());
                    break;
                case Protocol.FB_LED_RGB:
                    payload = Protocol.fbLedRgb(f.get("led").getAsInt(), f.get("r").getAsInt(), f.get("g").getAsInt(),
                            f.get("b").getAsInt(), (f.get("flags").getAsInt() & 1) != 0);
                    break;
                case Protocol.FB_LEDS: {
                    JsonArray lv = f.getAsJsonArray("levels");
                    int[] levels = new int[lv.size()];
                    for (int i = 0; i < levels.length; i++) levels[i] = lv.get(i).getAsInt();
                    payload = Protocol.fbLeds(f.get("first").getAsInt(), levels);
                    break;
                }
                case Protocol.FB_VALUE:
                    payload = Protocol.fbValue(f.get("kind").getAsInt(), f.get("index").getAsInt(), f.get("value").getAsInt());
                    break;
                case Protocol.FB_RING: {
                    int flags = f.get("flags").getAsInt();
                    payload = Protocol.fbRing(f.get("encoder").getAsInt(), f.get("mode").getAsInt(),
                            f.get("value").getAsInt(), (flags & 1) != 0, (flags & 2) != 0);
                    break;
                }
                case Protocol.FB_TEXT:
                    payload = Protocol.fbText(f.get("display").getAsInt(), f.get("row").getAsInt(),
                            f.get("col").getAsInt(), f.get("text").getAsString());
                    break;
                case Protocol.FB_RELEASE:
                    payload = Protocol.fbRelease(f.get("what").getAsInt());
                    break;
                default:
                    throw new AssertionError("unexpected notification " + cmd);
            }
            String desc = o.get("desc").getAsString();
            assertArrayEquals(hex(o.get("payload").getAsString()), payload, desc);
            assertArrayEquals(hex(o.get("frame").getAsString()), Protocol.buildFrame(cmd, 0, payload), desc);
            n++;
        }
        assertEquals(11, n);
        assertThrows(IllegalArgumentException.class, () -> Protocol.fbValue(Protocol.KIND_BUTTON, 0, 0));
        assertThrows(IllegalArgumentException.class, () -> Protocol.fbLeds(0, new int[65]));
        assertThrows(IllegalArgumentException.class, () -> Protocol.fbLed(0, 128));
    }

    @Test
    void hexRoundTrip() {
        byte[] b = Protocol.bytes(0xF0, 0x7D, 0x00, 0xF7);
        assertEquals("f07d00f7", Protocol.toHex(b));
        assertArrayEquals(b, Protocol.fromHex("F0 7D 00 F7"));
        assertArrayEquals(b, Protocol.fromHex("f07d00f7"));
    }
}
