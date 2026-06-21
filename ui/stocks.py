import plotly.graph_objects as go

from datetime import datetime


# --- build 1s candles straight from the trade log ---
secs = []   # epoch second for each candle

opens = []
his = []
lows = []
closes = []

last = 0
now = None

with open("../f") as f:
    for x in f:
        cmd = x.split(" ")

        if cmd[0] == "TRADE:":
            last = int(cmd[2]) / 100   # cents -> dollars
            if not secs:
                continue
            if opens[-1] == -1:
                # first trade of the bucket defines open and seeds the range
                opens[-1] = last
                his[-1] = last
                lows[-1] = last
            closes[-1] = last
            if last > his[-1]:
                his[-1] = last
            if last < lows[-1]:
                lows[-1] = last

        elif cmd[0] == "NOW":
            s = int(cmd[1]) // 1000000000   # whole-second bucket
            if s != now:
                now = s
                secs.append(s)
                opens.append(-1)
                his.append(last)
                lows.append(last)
                closes.append(last)

# drop seconds with zero trades (open never set) so empty windows stay blank
keep = [i for i in range(len(secs)) if opens[i] != -1]
secs = [secs[i] for i in keep]
opens = [opens[i] for i in keep]
his = [his[i] for i in keep]
lows = [lows[i] for i in keep]
closes = [closes[i] for i in keep]


# --- roll the 1s candles up into 1min candles ---
m_secs = []
m_opens = []
m_his = []
m_lows = []
m_closes = []

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
    else:
        if his[i] > m_his[-1]:
            m_his[-1] = his[i]
        if lows[i] < m_lows[-1]:
            m_lows[-1] = lows[i]
        m_closes[-1] = closes[i]     # close = last 1s close in the minute


# --- roll the 1min candles up into 1hour candles ---
h_secs = []
h_opens = []
h_his = []
h_lows = []
h_closes = []

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
    else:
        if m_his[i] > h_his[-1]:
            h_his[-1] = m_his[i]
        if m_lows[i] < h_lows[-1]:
            h_lows[-1] = m_lows[i]
        h_closes[-1] = m_closes[i]   # close = last 1min close in the hour


def show(secs, opens, his, lows, closes):
    fig = go.Figure(data=[go.Candlestick(
        x=[datetime.fromtimestamp(s) for s in secs],
        open=opens, high=his, low=lows, close=closes)])
    fig.show()


show(secs, opens, his, lows, closes)             # 1s candles
show(m_secs, m_opens, m_his, m_lows, m_closes)   # 1min candles
show(h_secs, h_opens, h_his, h_lows, h_closes)   # 1hour candles
