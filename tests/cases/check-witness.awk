function reject(message) {
    print FILENAME ": " message > "/dev/stderr"
    failed = 1
    exit 1
}

function save_frame(   name) {
    if (frame_seen)
        for (name in current)
            value[name, frame] = current[name]
}

function bit(name, t,   result) {
    result = value[name, t]
    if (result !~ /^[01]$/)
        reject("missing Boolean " name " at #" t)
    return result + 0
}

function closed(name) {
    if (!((name, loop_start) in value) || !((name, loop_end) in value) ||
        value[name, loop_start] != value[name, loop_end])
        reject("loop endpoints disagree for " name)
}

function unsigned_binary(bits,   n, i) {
    if (bits !~ /^[01]+$/)
        reject("missing definite binary value")
    n = 0
    for (i = 1; i <= length(bits); ++i)
        n = 2 * n + substr(bits, i, 1)
    return n
}

$1 == "$var" { name_of[$4] = $5 }
$1 == "$comment" && $2 == "witness" && $3 == "loop" {
    loop_start = $4 + 0
    loop_end = $6 + 0
    has_loop = 1
}
$0 == "$comment finite witness with proved total extension $end" { total_extension = 1 }
/^#[0-9]+$/ {
    save_frame()
    frame = substr($0, 2) + 0
    frame_seen = 1
}
/^[01xz]/ {
    id = substr($0, 2)
    if (id in name_of)
        current[name_of[id]] = substr($0, 1, 1)
}
/^b[01xz]+ / && ($2 in name_of) { current[name_of[$2]] = substr($1, 2) }

END {
    if (failed) exit 1
    save_frame()
    if (!frame_seen) reject("missing waveform frames")
    if ((require_certificate && !has_loop && !total_extension) || (has_loop && total_extension))
        reject("missing or ambiguous extension certificate")
    if (has_loop && (loop_start < 0 || loop_start >= loop_end || loop_end != frame))
        reject("invalid loop endpoints")

    if (fixture == "reset-low") {
        if (bit("rst_n", 0) != 0) reject("expected active-low reset at #0")
        first_bad = -1
        for (t = 1; t <= frame; ++t) {
            if (bit("q", t) != bit("rst_n", t - 1))
                reject("q does not follow the previous reset input at #" t)
            if (first_bad == -1 && bit("rst_n", t) && bit("q", t)) first_bad = t
        }
        if (first_bad != 2) reject("expected first failure at #2, got #" first_bad)
        if (has_loop) {
            closed("rst_n")
            closed("q")
        }
    } else if (fixture == "buechi") {
        bad_frame = choose_next ? 1 : 2
        if (bit("rst", 0) != 1 || bit("choose_next", 0) != choose_next)
            reject("wrong initial reset or target choice")
        if (bit("ready", bad_frame) != 0) reject("missing violation at #" bad_frame)
        if (bit("started", 0) != 0 || bit("ready", 1) != 0)
            reject("initial state or next-cycle assumption violated")
        for (t = 1; t <= frame; ++t)
            if (!bit("started", t) || bit("rst", t))
                reject("unexpected reset or state at #" t)
        if (has_loop) {
            closed("rst")
            closed("ready")
            closed("choose_next")
            closed("started")
        }
    } else if (fixture == "ram") {
        first_bad = -1
        for (i = 0; i < 8; ++i) expected[i] = 0
        for (t = 0; t <= frame; ++t) {
            count = t % 8
            if (unsigned_binary(value["c", t]) != count)
                reject("wrong RAM address at #" t)
            mem = value["mem", t]
            if (length(mem) != 64) reject("missing RAM contents at #" t)
            for (i = 0; i < 8; ++i)
                if (unsigned_binary(substr(mem, 1 + 8 * i, 8)) != expected[i])
                    reject("wrong mem[" i "] at #" t)
            if (first_bad == -1 && unsigned_binary(substr(mem, 41, 8)) != 0) first_bad = t
            expected[count] = count + 1
        }
        if (first_bad != 6 || first_bad > bound)
            reject("expected first mem[5] violation at #6 within bound " bound)
        if (has_loop) {
            closed("clk")
            closed("c")
            closed("mem")
        } else if (frame != first_bad) {
            reject("finite RAM witness must end at the first violation")
        }
    } else if (fixture == "terminal") {
        for (t = 0; t <= frame; ++t) {
            if (!bit("trip", t)) continue
            if (!violated && bound && t > bound)
                reject("first trip is outside the BMC bound")
            violated = 1
            future = t + delay
            if (has_loop && future >= loop_end)
                future = loop_start + (future - loop_start) % (loop_end - loop_start)
            if (future <= frame && !bit("alarm", future))
                reject("trip at #" t " is missing its alarm at #" future)
        }
        if (!violated) reject("missing trip in witness")
        if (has_loop) {
            closed("trip")
            closed("alarm")
        }
    } else if (fixture == "vacuity-buechi") {
        if (bit("rst", 0) != 1) reject("expected active-high reset at #0")
        for (t = 0; t < frame; ++t)
            if (bit("rst", t) && bit("ready", t + 1))
                reject("next-cycle assumption violated at #" (t + 1))
        for (t = 0; t + delay <= frame; ++t)
            if (bit("rst", t) && !bit("ready", t + delay)) violated = 1
        if (!violated) reject("missing target violation with delay " delay)
        if (has_loop) {
            closed("rst")
            closed("ready")
        }
    } else {
        reject("unknown witness fixture")
    }
}
