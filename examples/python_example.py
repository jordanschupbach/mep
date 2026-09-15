import numpy as np
import matplotlib.pyplot as plt

# <leader>uu on this file opens the Python language UI mode; <leader>rr then
# sources it into that console. Every top-level name lands in the Objects
# tab, plots land in the Plot tab, help() in the Help tab, mep_view() in Data.

np.array((1, 2, 3))
print("hello, world")

n = 1000
x = np.random.uniform(0, 1, n)
y = np.sin(20 / (x + 0.25)) + np.random.normal(0, 0.25, n)
plt.scatter(x, y, s=4)
xseq = np.linspace(0, 1, 1000)
plt.plot(xseq, np.sin(20 / (xseq + 0.25)), color="red")
plt.show()

plt.hist(y, bins=40)
plt.show()

help(np.linspace)
mep_view(x)
