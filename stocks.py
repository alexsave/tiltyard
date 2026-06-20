import matplotlib.pyplot as plt

X = []
Y = []

opens = []
his = []
lows = []
close = []

last = 0
current_open = 0
current_hi = 0
current_lows = 0
current_close = 0
current_s = 0

now = 0

with open("f") as f:
    for x in f:
        cmd = x.split(" ")
        if cmd[0] == "TRADE:":
            price = int(cmd[2])
            last = price
            if price > current_hi:
                current_hi = price
            if price < current_lows:
                current_lows = price
            #X.append(now)
            #Y.append(int(price))
            #print(price)
        elif cmd[0] == "NOW":
            
            ns = cmd[1]
            now = int(ns)
            s = int(now/1000000000)
            print(s)
            if s != current_s:
                X.append(current_s)
                opens.append(current_open)
                close.append(last)
                current_open = last
                his.append(current_hi)
                lows.append(current_lows)
                current_hi = last
                current_lows = last
                current_s = s

#close.append(last)
            
            #print(ns)

plt.scatter(X, his)
#plt.scatter(X, opens)
plt.scatter(X, lows)

#plt.scatter(opens,Y)
#plt.scatter(his,Y)
#plt.scatter(lows,Y)
#plt.scatter(close,Y)

plt.show()

