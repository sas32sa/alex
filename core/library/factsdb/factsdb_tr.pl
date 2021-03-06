:- module(factsdb_tr,[factsdb_exp/2],[]).

:- use_module(engine(messages_basic), [message/2]).

factsdb_exp((:- facts(F/A,File)),[C1,C2]):-
    atom(F),
    number(A),
    atom(File), !,
    functor(H,F,A),
    C1=(H:-factsdb_rt:call(H)),
    C2=('$factsdb$cached_goal'(H,H,File)).
factsdb_exp((:- facts(FA,F)),_):-
    message(warning, ['Wrong declaration: :- ',''(facts(FA,F))]).
