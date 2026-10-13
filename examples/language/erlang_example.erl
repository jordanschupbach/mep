%% A small Erlang tour: records, pattern-matched function clauses, list
%% comprehensions, and lists:foldl. Run as an escript, so main/1 is the entry
%% point and no separate erlc compile is needed.
-module(erlang_example).
-export([main/1]).

-record(point, {x = 0, y = 0}).

distance(#point{x = X1, y = Y1}, #point{x = X2, y = Y2}) ->
    math:sqrt(math:pow(X1 - X2, 2) + math:pow(Y1 - Y2, 2)).

area({circle, R}) -> math:pi() * R * R;
area({rect, W, H}) -> W * H.

main(_Args) ->
    io:format("hello, world~n"),
    A = #point{x = 0, y = 0},
    B = #point{x = 3, y = 4},
    io:format("distance = ~.2f~n", [distance(A, B)]),
    Nums = [1, 2, 3, 4, 5],
    io:format("sum = ~p~n", [lists:foldl(fun(N, Acc) -> N + Acc end, 0, Nums)]),
    io:format("squares = ~p~n", [[N * N || N <- Nums]]),
    io:format("evens = ~p~n", [[N || N <- Nums, N rem 2 =:= 0]]),
    [io:format("~p area = ~.2f~n", [element(1, S), float(area(S))])
     || S <- [{circle, 1}, {rect, 2, 3}]],
    ok.
