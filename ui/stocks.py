import plotly.graph_objects as go
from plotly.subplots import make_subplots

from datetime import datetime


# --- build 1s candles straight from the trade log ---
# log line format:
#   TRADE <side> <price> <qty> <order_id> <ts_ns> [PARTIAL]
# side is 0=sell / 1=buy; the timestamp now lives on the trade itself, so we
# bucket each trade by its own whole second (no separate NOW lines anymore).
secs = []   # epoch second for each candle

opens = []
his = []
lows = []
closes = []
vols = []   # total qty traded in each candle

now = None

with open("../f") as f:
    for x in f:
        cmd = x.split()

        if len(cmd) < 6 or cmd[0] != "TRADE":
            continue

        price = int(cmd[2]) / 100        # cents -> dollars
        qty = int(cmd[3])                # qty traded
        s = int(cmd[5]) // 1000000000    # ts ns -> whole-second bucket

        if s != now:
            # new second: trade seeds open/high/low/close for the bucket
            now = s
            secs.append(s)
            opens.append(price)
            his.append(price)
            lows.append(price)
            closes.append(price)
            vols.append(qty)
        else:
            closes[-1] = price
            if price > his[-1]:
                his[-1] = price
            if price < lows[-1]:
                lows[-1] = price
            vols[-1] += qty


# --- roll the 1s candles up into 1min candles ---
m_secs = []
m_opens = []
m_his = []
m_lows = []
m_closes = []
m_vols = []

cur = None
for i in range(len(secs)):
    m = secs[i] // 60
    if m != cur:
        cur = m
        m_secs.append(m * 60)       # epoch second of the minute boundary
        m_opens.append(opens[i])    # open = first 1s open in the minute
        m_his.append(his[i])
        m_lows.append(lows[i])
        m_closes.append(closes[i])
        m_vols.append(vols[i])
    else:
        if his[i] > m_his[-1]:
            m_his[-1] = his[i]
        if lows[i] < m_lows[-1]:
            m_lows[-1] = lows[i]
        m_closes[-1] = closes[i]     # close = last 1s close in the minute
        m_vols[-1] += vols[i]        # volume = sum of 1s volumes in the minute


# --- roll the 1min candles up into 1hour candles ---
h_secs = []
h_opens = []
h_his = []
h_lows = []
h_closes = []
h_vols = []

cur = None
for i in range(len(m_secs)):
    h = m_secs[i] // 3600
    if h != cur:
        cur = h
        h_secs.append(h * 3600)      # epoch second of the hour boundary
        h_opens.append(m_opens[i])   # open = first 1min open in the hour
        h_his.append(m_his[i])
        h_lows.append(m_lows[i])
        h_closes.append(m_closes[i])
        h_vols.append(m_vols[i])
    else:
        if m_his[i] > h_his[-1]:
            h_his[-1] = m_his[i]
        if m_lows[i] < h_lows[-1]:
            h_lows[-1] = m_lows[i]
        h_closes[-1] = m_closes[i]   # close = last 1min close in the hour
        h_vols[-1] += m_vols[i]      # volume = sum of 1min volumes in the hour


def show(secs, opens, his, lows, closes, vols):
    x = [datetime.fromtimestamp(s) for s in secs]

    # color each volume bar to match its candle (green up, red down)
    bar_colors = ["#2ca02c" if c >= o else "#d62728"
                  for o, c in zip(opens, closes)]

    fig = make_subplots(
        rows=2, cols=1, shared_xaxes=True,
        row_heights=[0.8, 0.2], vertical_spacing=0.02)

    fig.add_trace(go.Candlestick(
        x=x, open=opens, high=his, low=lows, close=closes,
        name="price"), row=1, col=1)
    fig.add_trace(go.Bar(
        x=x, y=vols, marker_color=bar_colors, name="volume"),
        row=2, col=1)

    fig.update_xaxes(rangeslider_visible=False, row=1, col=1)
    fig.update_yaxes(title_text="volume", row=2, col=1)
    fig.show()


show(secs, opens, his, lows, closes, vols)                 # 1s candles
show(m_secs, m_opens, m_his, m_lows, m_closes, m_vols)     # 1min candles
show(h_secs, h_opens, h_his, h_lows, h_closes, h_vols)     # 1hour candles
