"""A least-squares line, written to be shown piecewise from a mepml document.

test.mepml includes this file whole, by line range and by region."""


def mean(xs):
    return sum(xs) / len(xs)


class Fit:
    """y = slope * x + intercept, fitted to the points given."""

    def __init__(self, xs, ys):
        self.xs, self.ys = list(xs), list(ys)
        self.slope, self.intercept = self.solve()

    # region model
    def solve(self):
        mx, my = mean(self.xs), mean(self.ys)
        sxy = sum((x - mx) * (y - my) for x, y in zip(self.xs, self.ys))
        sxx = sum((x - mx) ** 2 for x in self.xs)
        slope = sxy / sxx
        return slope, my - slope * mx
    # endregion

    def predict(self, x):
        return self.slope * x + self.intercept


if __name__ == "__main__":
    fit = Fit([1, 2, 3, 4], [2.1, 3.9, 6.2, 7.8])
    print(f"slope {fit.slope:.2f}, intercept {fit.intercept:.2f}")
