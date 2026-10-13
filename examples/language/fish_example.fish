# A small fish tour: functions with named arguments, lists, math, string
# helpers, and switch.

function distance --argument-names x1 y1 x2 y2
    math --scale 2 "sqrt(($x1 - $x2)^2 + ($y1 - $y2)^2)"
end

function sum
    set -l total 0
    for n in $argv
        set total (math $total + $n)
    end
    echo $total
end

echo "hello, world"
echo "distance = "(distance 0 0 3 4)

set nums (seq 5)
echo "sum = "(sum $nums)

set squares
for n in $nums
    set -a squares (math $n \* $n)
end
echo "squares = "(string join ", " $squares)

for word in apple banana cherry
    switch $word
        case 'a*'
            echo "$word starts with a vowel"
        case '*'
            echo "$word is "(string length $word)" letters long"
    end
end
