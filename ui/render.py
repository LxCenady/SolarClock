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


def render_search(hb):
    """GNSS搜星中状态渲染(未定位)"""
    v = hb.get("v", 0)
    sats = hb.get("sats", 0)
    lines = [
        " SolarClock  GNSS acquiring",
        "",
        f" waiting for fix...  V{frames(v)}  sats {sats}",
        "",
        " (outdoor/open-sky for first fix)",
    ]
    return "\x1b[H" + "\n".join(lines) + "\n\x1b[?25l"


def frames(v):
    return str(v)


def render(hb):
    """hb: 心跳dict -> ANSI多行字符串(固定行数, 无闪烁)"""
    p = hb.get("p", 0)
    polar = p != 0
    dp = hb.get("dp", 0)
    ev = hb.get("ev", 0)
    ne = hb.get("ne", 0)
    tne = hb.get("tne", 0)

    rise = set_ = ""
    if polar:
        txt = _polar_text(p)
        rise = set_ = txt
    else:
        rise = hb.get("r", "--")
        set_ = hb.get("st", "--")

    if ne == 0:
        nxt = f"sunset in  {tne} min"
    elif ne == 1:
        nxt = f"sunrise in {tne} min"
    elif ne == 2:
        nxt = "n/a (polar day)"
    else:
        nxt = "n/a (polar night)"

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

    syn = hb.get("syn", -2)
    if syn == -1:
        syn_txt = "SYNCING"
    elif syn == -2:
        syn_txt = "NO SYNC"
    elif syn >= 3600:
        syn_txt = f"SYNCED {syn // 3600}h AGO"
    elif syn >= 60:
        syn_txt = f"SYNCED {syn // 60}m AGO"
    else:
        syn_txt = f"SYNCED {syn}s AGO"

    lines = [
        f" SolarClock  {d}  {w}",
        f" Solar time  {s}",
        f" Sunrise     {rise}",
        f" Sunset      {set_}",
        f" Sun         ALT {hb.get('alt', 0)}°  AZ {hb.get('az', 0)}°",
        f" Daylight    {_bar(dp)} {dp:3d}%",
        f" Next event  {nxt}",
        f" Event       {event}",
        f" Sync        {syn_txt}",
        f" Location    {loc}",
    ]
    return "\x1b[H" + "\n".join(lines) + "\n\x1b[?25l"
