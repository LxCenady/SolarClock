"""Render: 纯ANSI终端渲染, 只消费心跳dict, 无算法/无串口逻辑
(后续换LCD时只替换本模块)
"""

_POLAR_DAY = "The Sun is ALWAYS there."
_POLAR_NIGHT = "The Sun Rises, but NOT TODAY"
_EV_SUNRISE = "It's the Sun, Again."
_EV_SUNSET = "Another Day Has Gone"

BAR_W = 20


def _bar(dp):
    n = round(dp / 100 * BAR_W)
    return "[" + "#" * n + "-" * (BAR_W - n) + "]"


def _polar_text(p):
    return _POLAR_DAY if p == 1 else _POLAR_NIGHT


def clear_screen():
    return "\x1b[2J\x1b[H"


def render(hb):
    """hb: 心跳dict -> ANSI多行字符串(固定行数, 无闪烁)"""
    p = hb.get("p", 0)
    polar = p != 0
    dp = hb.get("dp", 0)
    ev = hb.get("ev", 0)

    rise = set_ = ""
    if polar:
        txt = _polar_text(p)
        rise = set_ = txt
    else:
        rise = hb.get("r", "--")
        set_ = hb.get("st", "--")

    ts = hb.get("ts", 0)
    sunset = "n/a (polar)" if polar else f"{ts} min"

    if ev == 1:
        event = _EV_SUNRISE
    elif ev == 2:
        event = _EV_SUNSET
    else:
        event = ""

    w = hb.get("t", "--:--:--")
    d = hb.get("d", "--")
    s = hb.get("s", "--:--")
    la, lo = hb.get("la", 0), hb.get("lo", 0)
    loc = f"{abs(la):.4f}{'N' if la >= 0 else 'S'} {abs(lo):.4f}{'E' if lo >= 0 else 'W'}"

    lines = [
        f" SolarClock  {d}  {w}",
        f" solar time  {s}",
        f" location    {loc}",
        f" sunrise     {rise}",
        f" sunset      {set_}",
        f" daylight    {_bar(dp)} {dp:3d}%",
        f" sunset in   {sunset}",
        f" event       {event}",
    ]
    return "\x1b[H" + "\n".join(lines) + "\n\x1b[?25l"
