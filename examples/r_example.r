library(splines)
print("Hello world")

n <- 1000
x <- runif(n, 0, 1)
y <- sin(20 / (x + 0.25)) + rnorm(n, 0, 0.25)
plot(x, y)
nknots <- 200
deg <- 3
k <- seq(0, 1, length.out = nknots)
xmat <- spline.des(k, x, deg, outer.ok = TRUE)$design
beta <- solve(t(xmat) %*% xmat) %*% t(xmat) %*% y
xseq <- seq(0, 1, length.out = 1000)
xseqmat <- spline.des(k, xseq, deg, outer.ok = TRUE)$design
ypred <- xseqmat %*% beta
lines(xseq, ypred, col = "red")




# j
