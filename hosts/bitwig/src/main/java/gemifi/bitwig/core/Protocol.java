// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Set;

/**
 * gemifi SysEx protocol v1 codec, the parts a DAW extension needs ({@code protocol/sysex.md}).
 *
 * <p>Pure Java, no Bitwig imports. A port of {@code hosts/ableton-live/gemifi/protocol.py}
 * (this project's own MIT code). Byte arrays hold 7-bit values; anything read is masked with
 * {@code 0xFF}.
 *
 * <pre>
 * Frame:  F0 7D 67 6D  ver cmd seq len_lo len_hi  payload...  chk  F7
 * chk = (sum of ver..last payload byte) &amp; 0x7F; len = payload length in SysEx bytes (u14).
 * </pre>
 */
public final class Protocol {
    private Protocol() {}

    /** MIDI non-commercial ID (sysex.md §1). Kept in this one constant, as the spec asks. */
    public static final byte[] MANUFACTURER = {0x7D};
    public static final byte[] SIGNATURE = {0x67, 0x6D};
    public static final byte[] HEADER = concat(new byte[] {(byte) 0xF0}, MANUFACTURER, SIGNATURE);
    public static final int PROTO_VERSION = 1;

    // Requests 0x00-0x1F, replies cmd|0x40, host->device notifications 0x20-0x3F, unsolicited 0x60-0x7F.
    public static final int HELLO = 0x01;
    public static final int GET_PARAM = 0x03;
    public static final int SET_PARAM = 0x04;
    public static final int GET_COMPONENT = 0x11;
    public static final int REPLY = 0x40;
    public static final int NOTIFY_FIRST = 0x20;
    public static final int NOTIFY_LAST = 0x3F;
    public static final int UNSOLICITED_FIRST = 0x60;
    public static final int LOG = 0x60;
    public static final int EVENT = 0x61;
    public static final int PARAM_CHANGED = 0x62;
    /** sysex.md §4.6.3: HELLO's fields, sent once after the device starts. */
    public static final int BOOTED = 0x64;

    // Host feedback notifications (sysex.md §4.6.2). Never answered.
    public static final int FB_LED = 0x20;
    public static final int FB_LED_RGB = 0x21;
    public static final int FB_LEDS = 0x22;
    public static final int FB_VALUE = 0x23;
    public static final int FB_RING = 0x24;
    public static final int FB_TEXT = 0x25;
    public static final int FB_RELEASE = 0x26;

    public static final int RELEASE_ALL = 0, RELEASE_LEDS = 1, RELEASE_VALUES = 2, RELEASE_TEXT = 3;
    public static final int RING_DOT = 0, RING_BOOST_CUT = 1, RING_WRAP = 2, RING_SPREAD = 3;

    public static final int OK = 0, BAD_CHECKSUM = 1, BAD_CMD = 2, BAD_LEN = 3, BAD_STATE = 4,
            BAD_ADDR = 5, FLASH_ERR = 6, BAD_IMAGE = 7, BAD_BOARD = 8, BUSY = 9, BAD_PARAM = 10,
            BAD_VALUE = 11;
    private static final String[] STATUS_NAMES = {"OK", "BAD_CHECKSUM", "BAD_CMD", "BAD_LEN",
        "BAD_STATE", "BAD_ADDR", "FLASH_ERR", "BAD_IMAGE", "BAD_BOARD", "BUSY", "BAD_PARAM",
        "BAD_VALUE"};

    public static String statusName(int status) {
        return status >= 0 && status < STATUS_NAMES.length ? STATUS_NAMES[status] : "status " + status;
    }

    public static final int CAP_FW = 1, CAP_CONFIG = 2, CAP_LOG = 4, CAP_EVENT = 8;
    public static final int CAP_PRESETS = 1 << 5;
    /** sysex.md §4.1 bit 6: host feedback. */
    public static final int CAP_FEEDBACK = 1 << 6;

    /** sysex.md §4.3 / §4.6 step 3 (VOLATILE: a device restart turns it off). */
    public static final int PARAM_DEBUG_EVENTS = 13;

    // Component kinds (sysex.md §4.6.1); 1..4 and 7 are also the EVENT types.
    public static final int KIND_BUTTON = 1, KIND_KEYS = 2, KIND_ANALOG = 3, KIND_ENCODER = 4,
            KIND_LED = 5, KIND_DISPLAY = 6, KIND_PAD = 7;
    /** Firmware from before pads answers BAD_PARAM for kind 7: no pads. */
    public static final int[] ALL_KINDS = {KIND_BUTTON, KIND_KEYS, KIND_ANALOG, KIND_ENCODER,
        KIND_LED, KIND_DISPLAY, KIND_PAD};

    public static String kindName(int kind) {
        switch (kind) {
            case KIND_BUTTON: return "button";
            case KIND_KEYS: return "keys";
            case KIND_ANALOG: return "analog";
            case KIND_ENCODER: return "encoder";
            case KIND_LED: return "led";
            case KIND_DISPLAY: return "display";
            case KIND_PAD: return "pad";
            default: return "kind " + kind;
        }
    }

    /** "None" in GET_COMPONENT extra fields (no LED, no centre LED). */
    public static final int NONE_INDEX = 0x3FFF;
    // Fields added to GET_COMPONENT after the first F3 firmware (sysex.md §4.6.1 "Added fields").
    // A Component holds null for one the device didn't send.
    public static final int TAKEOVER_NONE = 0, TAKEOVER_PICKUP = 1;
    public static final int ENC_ABSOLUTE = 0, ENC_REL_TWOS = 1, ENC_REL_OFFSET = 2, ENC_MCU = 3;
    public static final int OWNER_HOST_THEN_LOCAL = 0, OWNER_HOST = 1, OWNER_LOCAL = 2;
    /**
     * EVENT type 8 (sysex.md §4.6.1 {@code layer}): a mode's layer turned on (value 1) or off (0);
     * index = the mode index. A twin with {@code layer} n is live while mode n − 1 is on.
     */
    public static final int EVENT_LAYER = 8;

    public static final int U14_MAX = 0x3FFF;
    public static final int LSTRING_MAX = 31;

    /** A malformed message. */
    public static final class ProtocolException extends Exception {
        private static final long serialVersionUID = 1L;

        public ProtocolException(String message) {
            super(message);
        }
    }

    // ---- small byte helpers ----

    public static byte[] concat(byte[]... parts) {
        int n = 0;
        for (byte[] p : parts) n += p.length;
        byte[] out = new byte[n];
        int pos = 0;
        for (byte[] p : parts) {
            System.arraycopy(p, 0, out, pos, p.length);
            pos += p.length;
        }
        return out;
    }

    public static byte[] bytes(int... values) {
        byte[] out = new byte[values.length];
        for (int i = 0; i < values.length; i++) out[i] = (byte) values[i];
        return out;
    }

    static int at(byte[] d, int i) {
        return d[i] & 0xFF;
    }

    // ---- 7-bit integer encodings (least significant group first, sysex.md §2) ----

    private static byte[] enc(long value, int nbytes, int bits) {
        if (value < 0 || value >= (1L << bits)) {
            throw new IllegalArgumentException(value + " does not fit in " + bits + " bits");
        }
        byte[] out = new byte[nbytes];
        for (int i = 0; i < nbytes; i++) out[i] = (byte) ((value >> (7 * i)) & 0x7F);
        return out;
    }

    private static long dec(byte[] d, int pos, int nbytes) throws ProtocolException {
        if (d.length - pos < nbytes) {
            throw new ProtocolException("need " + nbytes + " bytes, got " + Math.max(0, d.length - pos));
        }
        long v = 0;
        for (int i = 0; i < nbytes; i++) {
            int b = at(d, pos + i);
            if ((b & 0x80) != 0) throw new ProtocolException("byte with bit 7 set inside SysEx");
            v |= ((long) b) << (7 * i);
        }
        return v;
    }

    public static byte[] u14(int v) {
        return enc(v, 2, 14);
    }

    public static byte[] u28(long v) {
        return enc(v, 4, 28);
    }

    public static byte[] u32(long v) {
        return enc(v, 5, 32);
    }

    /** Signed 32-bit as the u32 of its two's-complement pattern (-1 → 7F 7F 7F 7F 0F). */
    public static byte[] s32(int v) {
        return u32(v & 0xFFFFFFFFL);
    }

    public static int decU14(byte[] d, int pos) throws ProtocolException {
        return (int) dec(d, pos, 2);
    }

    public static long decU28(byte[] d, int pos) throws ProtocolException {
        return dec(d, pos, 4);
    }

    public static long decU32(byte[] d, int pos) throws ProtocolException {
        long v = dec(d, pos, 5);
        if ((v >> 32) != 0) throw new ProtocolException("u32 top byte has bits above bit 3 set");
        return v;
    }

    public static int decS32(byte[] d, int pos) throws ProtocolException {
        return (int) decU32(d, pos);
    }

    /** A 14-bit two's-complement value (encoder EVENT delta) as an int. */
    public static int s14ToInt(int v) {
        v &= U14_MAX;
        return (v & 0x2000) != 0 ? v - 0x4000 : v;
    }

    public static int intToS14(int v) {
        if (v < -0x2000 || v >= 0x2000) throw new IllegalArgumentException(v + " does not fit in s14");
        return v & U14_MAX;
    }

    // ---- 8->7 packing (Korg/Elektron: MSB byte first, then up to 7 low-7-bit bytes) ----

    public static byte[] pack7(byte[] data) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (int i = 0; i < data.length; i += 7) {
            int n = Math.min(7, data.length - i);
            int msbs = 0;
            for (int j = 0; j < n; j++) msbs |= ((at(data, i + j) >> 7) & 1) << j;
            out.write(msbs);
            for (int j = 0; j < n; j++) out.write(at(data, i + j) & 0x7F);
        }
        return out.toByteArray();
    }

    public static byte[] unpack7(byte[] data) throws ProtocolException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (int i = 0; i < data.length; i += 8) {
            int n = Math.min(8, data.length - i);
            if (n < 2) throw new ProtocolException("packed data ends with a lone MSB byte");
            for (int j = 0; j < n; j++) {
                if ((at(data, i + j) & 0x80) != 0) throw new ProtocolException("byte with bit 7 set inside SysEx");
            }
            int msbs = at(data, i);
            for (int j = 1; j < n; j++) out.write(at(data, i + j) | (((msbs >> (j - 1)) & 1) << 7));
        }
        return out.toByteArray();
    }

    // ---- lstring (sysex.md §2: u8 len 0..31, printable ASCII, unpacked) ----

    public static byte[] encLstring(String text) {
        byte[] raw = text.getBytes(StandardCharsets.US_ASCII);
        int n = Math.min(raw.length, LSTRING_MAX);
        byte[] out = new byte[n + 1];
        out[0] = (byte) n;
        for (int i = 0; i < n; i++) {
            int b = at(raw, i);
            out[i + 1] = (byte) (b >= 0x20 && b <= 0x7E ? b : '?');
        }
        return out;
    }

    /** Decodes an lstring at {@code pos[0]} and advances it. Non-printable bytes come back as '?'. */
    public static String decLstring(byte[] d, int[] pos) throws ProtocolException {
        int p = pos[0];
        if (p >= d.length) throw new ProtocolException("lstring: missing length byte");
        int n = at(d, p);
        if (n > LSTRING_MAX) throw new ProtocolException("lstring length " + n + " > " + LSTRING_MAX);
        int end = p + 1 + n;
        if (end > d.length) throw new ProtocolException("lstring runs past the payload");
        StringBuilder sb = new StringBuilder(n);
        for (int i = p + 1; i < end; i++) {
            int b = at(d, i);
            sb.append(b >= 0x20 && b <= 0x7E ? (char) b : '?');
        }
        pos[0] = end;
        return sb.toString();
    }

    // ---- framing ----

    public static int checksum(byte[] body, int from, int to) {
        int sum = 0;
        for (int i = from; i < to; i++) sum += at(body, i);
        return sum & 0x7F;
    }

    /** The complete SysEx message (F0 .. F7). */
    public static byte[] buildFrame(int cmd, int seq, byte[] payload) {
        for (byte b : payload) {
            if ((b & 0x80) != 0) throw new IllegalArgumentException("payload must be 7-bit");
        }
        if (cmd < 0 || cmd > 0x7F || seq < 0 || seq > 0x7F) {
            throw new IllegalArgumentException("cmd and seq must be 0..127");
        }
        byte[] body = concat(bytes(PROTO_VERSION, cmd, seq), u14(payload.length), payload);
        return concat(HEADER, body, bytes(checksum(body, 0, body.length), 0xF7));
    }

    /** One parsed frame. */
    public static final class Frame {
        public final int ver;
        public final int cmd;
        public final int seq;
        public final byte[] payload;

        public Frame(int ver, int cmd, int seq, byte[] payload) {
            this.ver = ver;
            this.cmd = cmd;
            this.seq = seq;
            this.payload = payload;
        }

        public boolean isReply() {
            return cmd >= REPLY && cmd < UNSOLICITED_FIRST;
        }

        public boolean isUnsolicited() {
            return cmd >= UNSOLICITED_FIRST;
        }

        public boolean isNotification() {
            return cmd >= NOTIFY_FIRST && cmd <= NOTIFY_LAST;
        }

        /** The reply status byte, or -1 for an empty payload. */
        public int status() {
            return payload.length > 0 ? at(payload, 0) : -1;
        }

        @Override
        public String toString() {
            return String.format("Frame(cmd=0x%02X seq=%d payload=%s)", cmd, seq, toHex(payload));
        }
    }

    public static boolean isGemifi(byte[] msg) {
        if (msg.length < HEADER.length) return false;
        for (int i = 0; i < HEADER.length; i++) {
            if (msg[i] != HEADER[i]) return false;
        }
        return true;
    }

    /** Parses one complete SysEx message (F0..F7). */
    public static Frame parseFrame(byte[] msg) throws ProtocolException {
        int h = HEADER.length;
        if (msg.length < h + 7 || at(msg, 0) != 0xF0 || at(msg, msg.length - 1) != 0xF7) {
            throw new ProtocolException("not a complete SysEx message");
        }
        if (!isGemifi(msg)) throw new ProtocolException("not a gemifi message");
        for (int i = 1; i < msg.length - 1; i++) {
            if ((msg[i] & 0x80) != 0) throw new ProtocolException("byte with bit 7 set inside SysEx");
        }
        int bodyEnd = msg.length - 2;            // exclusive; msg[bodyEnd] = chk
        int ver = at(msg, h), cmd = at(msg, h + 1), seq = at(msg, h + 2);
        int n = decU14(msg, h + 3);
        int payloadStart = h + 5;
        int have = bodyEnd - payloadStart;
        if (have != n) throw new ProtocolException("length field " + n + " but payload has " + have + " bytes");
        if (checksum(msg, h, bodyEnd) != at(msg, bodyEnd)) throw new ProtocolException("bad checksum");
        if (ver != PROTO_VERSION) throw new ProtocolException("unsupported protocol version " + ver);
        return new Frame(ver, cmd, seq, Arrays.copyOfRange(msg, payloadStart, bodyEnd));
    }

    // ---- hex (Bitwig passes SysEx as hex strings) ----

    private static final char[] HEX = "0123456789abcdef".toCharArray();

    public static String toHex(byte[] data) {
        char[] out = new char[data.length * 2];
        for (int i = 0; i < data.length; i++) {
            out[2 * i] = HEX[(data[i] >> 4) & 0xF];
            out[2 * i + 1] = HEX[data[i] & 0xF];
        }
        return new String(out);
    }

    /** Hex text (spaces allowed, any case) to bytes. */
    public static byte[] fromHex(String hex) {
        // One pass, no regex or substrings: this runs for every SysEx message the device sends.
        int len = hex.length();
        byte[] out = new byte[len / 2];
        int n = 0;
        int high = -1;
        for (int i = 0; i < len; i++) {
            char ch = hex.charAt(i);
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\u000B') continue;
            int d = Character.digit(ch, 16);
            if (d < 0) throw new NumberFormatException("not a hex digit: '" + ch + "'");
            if (high < 0) {
                high = d;
            } else {
                out[n++] = (byte) (high << 4 | d);
                high = -1;
            }
        }
        if (high >= 0) throw new IllegalArgumentException("odd-length hex string");
        return n == out.length ? out : Arrays.copyOf(out, n);
    }

    // ---- HELLO ----

    /** status + role, board_id u14, fw_version u32, proto, max_chunk u14, caps u14, uid (14 packed). */
    public static final int HELLO_REPLY_LEN = 1 + 1 + 2 + 5 + 1 + 2 + 2 + 14;

    /** A HELLO reply. */
    public static final class Hello {
        public final int role;
        public final int boardId;
        public final long fwVersion;
        public final int protoVersion;
        public final int maxChunk;
        public final int caps;
        public final byte[] uid;
        /** The YAML node name, or null (§4.1.1). */
        public final String node;

        public Hello(int role, int boardId, long fwVersion, int protoVersion, int maxChunk, int caps,
                byte[] uid, String node) {
            this.role = role;
            this.boardId = boardId;
            this.fwVersion = fwVersion;
            this.protoVersion = protoVersion;
            this.maxChunk = maxChunk;
            this.caps = caps;
            this.uid = uid;
            this.node = node;
        }

        public boolean hasFeedback() {
            return (caps & CAP_FEEDBACK) != 0;
        }

        public String versionText() {
            return (fwVersion >> 24) + "." + ((fwVersion >> 16) & 0xFF) + "." + (fwVersion & 0xFFFF);
        }
    }

    static boolean validNode(String name) {
        if (name.length() < 1 || name.length() > 31 || name.startsWith("-") || name.endsWith("-")) return false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        }
        return true;
    }

    /** HELLO reply payload (status OK) → Hello. Trailing bytes past the known fields are ignored (§4.1.1). */
    public static Hello parseHello(byte[] payload) throws ProtocolException {
        if (payload.length < HELLO_REPLY_LEN) {
            throw new ProtocolException("HELLO reply is " + payload.length + " bytes, expected at least " + HELLO_REPLY_LEN);
        }
        String node = null;
        int rest = HELLO_REPLY_LEN;
        if (payload.length > rest) {
            int n = at(payload, rest);
            if (n > 0 && rest + 1 + n <= payload.length) {
                String name = new String(payload, rest + 1, n, StandardCharsets.US_ASCII);
                node = validNode(name) ? name : null;
            }
        }
        return new Hello(at(payload, 1), decU14(payload, 2), decU32(payload, 4), at(payload, 9),
                decU14(payload, 10), decU14(payload, 12),
                unpack7(Arrays.copyOfRange(payload, 14, 28)), node);
    }

    /** BOOTED payload (sysex.md §4.6.3) → Hello: a HELLO reply without its status byte. */
    public static Hello parseBooted(byte[] payload) throws ProtocolException {
        return parseHello(concat(bytes(OK), payload));
    }

    /** Device side (tests, fake device). */
    public static byte[] buildHelloReply(Hello h) {
        byte[] node = h.node == null ? new byte[0]
                : concat(bytes(h.node.length()), h.node.getBytes(StandardCharsets.US_ASCII));
        return concat(bytes(OK, h.role), u14(h.boardId), u32(h.fwVersion), bytes(h.protoVersion),
                u14(h.maxChunk), u14(h.caps), pack7(h.uid), node);
    }

    // ---- GET_PARAM / SET_PARAM / PARAM_CHANGED ----

    public static byte[] reqGetParam(int id, int index) {
        return concat(u14(id), u14(index));
    }

    public static byte[] reqSetParam(int id, int index, int value) {
        return concat(u14(id), u14(index), s32(value));
    }

    /** {id, index, value}. Skips the status byte of a reply (10 bytes); PARAM_CHANGED has none (9). */
    public static int[] parseParamReply(byte[] payload) throws ProtocolException {
        int off;
        if (payload.length == 10) off = 1;
        else if (payload.length == 9) off = 0;
        else throw new ProtocolException("param payload must be 9 bytes after the status");
        return new int[] {decU14(payload, off), decU14(payload, off + 2), decS32(payload, off + 4)};
    }

    public static byte[] buildParamReply(int id, int index, int value) {
        return concat(bytes(OK), u14(id), u14(index), s32(value));
    }

    // ---- GET_COMPONENT (sysex.md §4.6.1) ----

    public static byte[] reqGetComponent(int kind, int index) {
        return concat(bytes(kind), u14(index));
    }

    /** One component from a GET_COMPONENT reply. {@code role} is set to the normalised role by the device layer. */
    public static final class Component {
        public int kind;
        public int index;
        public int count;
        public String id = "";
        public String name = "";
        public String role = "";
        public int led = NONE_INDEX;
        public int keys;
        public int firstNote;
        public int ring;
        public int center = NONE_INDEX;
        public int colours = 1;
        public int cols;
        public int rows;
        public int displayType;
        /** Analog: 0 none, 1 pick-up; null = not reported (older firmware). */
        public Integer takeover;
        /** Encoder: 0 absolute, 1/2 relative, 3 Mackie V-Pot (non-zero = relative); null = not reported. */
        public Integer mode;
        /** LED: 0 host_then_local, 1 host, 2 local; null = not reported. */
        public Integer owner;
        /**
         * Button, encoder, LED, pad: 0 always there, n = a twin in mode n − 1 (live only while that mode
         * is on); null = not reported (older firmware: treat as 0).
         */
        public Integer layer;

        public Component() {}

        public Component(int kind, int index, int count, String id, String name, String role) {
            this.kind = kind;
            this.index = index;
            this.count = count;
            this.id = id;
            this.name = name;
            this.role = role;
        }

        public boolean hasLed() {
            return (kind == KIND_BUTTON || kind == KIND_PAD) && led != NONE_INDEX;
        }

        /**
         * Whether FB_VALUE does anything (sysex.md §4.6.2): a pick-up analog input or an absolute
         * encoder. True when the device didn't say, as the extension assumed before the field existed.
         */
        public boolean takesValue() {
            if (kind == KIND_ANALOG) return takeover == null || takeover == TAKEOVER_PICKUP;
            if (kind == KIND_ENCODER) return mode == null || mode == ENC_ABSOLUTE;
            return false;
        }

        /** An LED with owner: local ignores every FB_* (sysex.md §4.6.2 "Ownership"). */
        public boolean localOnly() {
            return kind == KIND_LED && owner != null && owner == OWNER_LOCAL;
        }

        /** The mode this component is a twin in, or -1 if it's always there. */
        public int modeIndex() {
            return layer == null || layer == 0 ? -1 : layer - 1;
        }

        /** Whether it's live, given the mode indexes whose layers are on (EVENT 8). */
        public boolean isLive(Set<Integer> layersOn) {
            int m = modeIndex();
            return m < 0 || layersOn.contains(m);
        }

        /** Whether the reply carried its kind's added field (so the firmware also sends BOOTED). */
        public boolean reportsAddedFields() {
            switch (kind) {
                case KIND_ANALOG: return takeover != null;
                case KIND_ENCODER: return mode != null;
                case KIND_LED: return owner != null;
                default: return false;
            }
        }

        @Override
        public String toString() {
            return "Component(" + kindName(kind) + " " + index + " '" + id + "' role=" + role + ")";
        }
    }

    public static Component parseComponentReply(byte[] payload) throws ProtocolException {
        if (payload.length < 6 || at(payload, 0) != OK) {
            throw new ProtocolException("GET_COMPONENT reply too short or not OK");
        }
        Component c = new Component();
        c.kind = at(payload, 1);
        c.index = decU14(payload, 2);
        c.count = decU14(payload, 4);
        int[] pos = {6};
        c.id = decLstring(payload, pos);
        c.name = decLstring(payload, pos);
        c.role = decLstring(payload, pos);
        int p = pos[0];
        int left = payload.length - p;
        String missing = "GET_COMPONENT " + kindName(c.kind) + " reply missing its extra fields";
        switch (c.kind) {
            case KIND_BUTTON:
                if (left < 2) throw new ProtocolException(missing);
                c.led = decU14(payload, p);
                c.layer = left > 2 ? at(payload, p + 2) : null;
                break;
            case KIND_KEYS:
                if (left < 3) throw new ProtocolException(missing);
                c.keys = decU14(payload, p);
                c.firstNote = at(payload, p + 2);
                break;
            case KIND_ANALOG:
                c.takeover = left > 0 ? at(payload, p) : null;
                break;
            case KIND_ENCODER:
                if (left < 3) throw new ProtocolException(missing);
                c.ring = at(payload, p);
                c.center = decU14(payload, p + 1);
                c.mode = left > 3 ? at(payload, p + 3) : null;
                c.layer = left > 4 ? at(payload, p + 4) : null;
                break;
            case KIND_LED:
                if (left < 1) throw new ProtocolException(missing);
                c.colours = at(payload, p);
                c.owner = left > 1 ? at(payload, p + 1) : null;
                c.layer = left > 2 ? at(payload, p + 2) : null;
                break;
            case KIND_DISPLAY:
                if (left < 3) throw new ProtocolException(missing);
                c.cols = at(payload, p);
                c.rows = at(payload, p + 1);
                c.displayType = at(payload, p + 2);
                break;
            case KIND_PAD:
                if (left < 2) throw new ProtocolException(missing);
                c.led = decU14(payload, p);
                c.layer = left > 2 ? at(payload, p + 2) : null;
                break;
            default:
                // unknown kinds: ignore whatever follows (takeover / mode / owner / layer are absent in older firmware)
                break;
        }
        return c;
    }

    /** Device side (tests, fake device). {@code layer} goes out only after the field before it. */
    public static byte[] buildComponentReply(Component c) {
        byte[] head = concat(bytes(OK, c.kind), u14(c.index), u14(c.count), encLstring(c.id),
                encLstring(c.name), encLstring(c.role));
        switch (c.kind) {
            case KIND_BUTTON:
            case KIND_PAD:
                return concat(head, u14(c.led), opt(c.layer));
            case KIND_KEYS: return concat(head, u14(c.keys), bytes(c.firstNote));
            case KIND_ANALOG: return concat(head, opt(c.takeover));
            case KIND_ENCODER:
                return concat(head, bytes(c.ring), u14(c.center), opt(c.mode), c.mode == null ? new byte[0] : opt(c.layer));
            case KIND_LED:
                return concat(head, bytes(c.colours), opt(c.owner), c.owner == null ? new byte[0] : opt(c.layer));
            case KIND_DISPLAY: return concat(head, bytes(c.cols, c.rows, c.displayType));
            default: return head;
        }
    }

    private static byte[] opt(Integer v) {
        return v == null ? new byte[0] : bytes(v);
    }

    // ---- EVENT (0x61) ----

    /**
     * An EVENT: type 1 button, 2 key, 3 analog (0..16383), 4 encoder (signed 14-bit delta), 7 pad
     * (a twin's hit, 0 = release), 8 layer.
     */
    public static final class Event {
        public final int type;
        public final int index;
        public final int value;
        public final long tMs;

        public Event(int type, int index, int value, long tMs) {
            this.type = type;
            this.index = index;
            this.value = value;
            this.tMs = tMs;
        }

        /** Encoder events: {@code value} as a signed delta (14-bit two's complement). */
        public int delta() {
            return s14ToInt(value);
        }

        @Override
        public String toString() {
            return "Event(type=" + type + " index=" + index + " value=" + value + ")";
        }
    }

    public static Event parseEvent(byte[] payload) throws ProtocolException {
        if (payload.length != 9) throw new ProtocolException("EVENT payload must be 9 bytes");
        return new Event(at(payload, 0), decU14(payload, 1), decU14(payload, 3), decU28(payload, 5));
    }

    public static byte[] buildEvent(int type, int index, int value, long tMs) {
        return concat(bytes(type), u14(index), u14(value & U14_MAX), u28(tMs));
    }

    // ---- FB_* payloads (sysex.md §4.6.2) ----

    private static byte[] b7(int v, String what) {
        if (v < 0 || v > 0x7F) throw new IllegalArgumentException(what + " must be 0..127, got " + v);
        return bytes(v);
    }

    public static byte[] fbLed(int led, int level) {
        return concat(u14(led), b7(level, "level"));
    }

    public static byte[] fbLedRgb(int led, int r, int g, int b, boolean blink) {
        return concat(u14(led), b7(r, "r"), b7(g, "g"), b7(b, "b"), bytes(blink ? 1 : 0));
    }

    public static byte[] fbLeds(int first, int[] levels) {
        if (levels.length < 1 || levels.length > 64) throw new IllegalArgumentException("FB_LEDS takes 1..64 levels");
        byte[] out = new byte[levels.length];
        for (int i = 0; i < levels.length; i++) out[i] = b7(levels[i], "level")[0];
        return concat(u14(first), bytes(levels.length), out);
    }

    public static byte[] fbValue(int kind, int index, int value) {
        if (kind != KIND_ANALOG && kind != KIND_ENCODER) {
            throw new IllegalArgumentException("FB_VALUE kind must be 3 (analog) or 4 (encoder)");
        }
        return concat(bytes(kind), u14(index), u14(value));
    }

    /** FB_RING; {@code off} (flags bit 1) turns every ring LED off. */
    public static byte[] fbRing(int encoder, int mode, int value, boolean centre, boolean off) {
        return concat(u14(encoder), b7(mode, "mode"), u14(value), bytes((centre ? 1 : 0) | (off ? 2 : 0)));
    }

    public static byte[] fbText(int display, int row, int col, String text) {
        byte[] raw = text.getBytes(StandardCharsets.US_ASCII);
        int n = Math.min(raw.length, 64);
        byte[] t = new byte[n];
        for (int i = 0; i < n; i++) {
            int b = at(raw, i);
            t[i] = (byte) (b >= 0x20 && b <= 0x7E ? b : 0x20);
        }
        return concat(u14(display), b7(row, "row"), b7(col, "col"), t);
    }

    public static byte[] fbRelease(int what) {
        return b7(what, "what");
    }
}
