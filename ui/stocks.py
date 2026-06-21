import plotly.graph_objects as go

from datetime import datetime



import matplotlib.pyplot as plt

X = []
Y = []

opens = []
his = []
lows = []
closes = []

last = 0
current_open = 0
current_hi = 0
current_lows = 0
current_close = 0
current_s = 0

now = 0

opens.append(0)
his.append(0)
lows.append(0)
closes.append(0)

with open("../f") as f:
    for x in f:
        cmd = x.split(" ")

        if cmd[0] == "TRADE:":
            last = int(cmd[2])
            if opens[-1] == -1:
                opens[-1] = last
            closes[-1] = last
            if last > his[-1]:
                his[-1] = last
            if last < lows[-1]:
                lows[-1] = last
            
        elif cmd[0] == "NOW":
            s = int(cmd[1])/1000000000
            if s != now:
                X.append(s)
                opens.append(-1)
                his.append(last)
                lows.append(last)
                closes.append(last)
                
            

fig = go.Figure(data=[go.Candlestick(x=X,
                open=opens,
                high=his,
                low=lows,
                close=closes)])


fig.show()


