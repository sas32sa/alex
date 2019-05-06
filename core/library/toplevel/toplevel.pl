:- module(_, [], [assertions, nortchecks, define_flag, datafacts]).

:- if(defined(optim_comp)).
:- use_package(hiord).
:- use_module(engine(hiord_rt), [this_module/1, call/1]).
:- use_package(compiler(complang_mini)).
:- use_package(compiler(compiler_object)).
:- else.
:- use_package(dcg).
:- use_module(engine(hiord_rt), [this_module/1, call/1, '$nodebug_call'/1]).
:- endif.

:- if(defined(optim_comp)).
% (all compile capabilities, e.g., compile from source, etc.)
:- use_module(compiler(all_actions), []).
:- endif.

% Dynamic module loading
:- if(defined(optim_comp)).
:- use_module(compiler(dynload), [use_module/3, ensure_loaded/2]).
:- else.
:- use_module(library(compiler), [use_module/3, ensure_loaded/2]).
:- endif.

% Create executables
:- if(defined(optim_comp)).
% TODO: fixme
:- else.
:- use_module(library(compiler/exemaker), [make_exec/2]).
:- endif.

:- use_module(library(debugger)).
%
:- use_module(library(toplevel/toplevel_io)).
:- use_module(engine(messages_basic), [message/2, message_lns/4]).
:- use_module(engine(stream_basic)).
:- use_module(engine(io_basic)).
:- use_module(library(stream_utils), [write_string/1]).
:- use_module(library(write), [write/1, write_term/2]).
%
:- use_module(engine(internals), ['$bootversion'/0, '$empty_gcdef_bin'/0, '$force_interactive'/0]).
:- use_module(library(lists), [member/2]).
:- use_module(library(libpaths), [get_alias_path/0]).
:- use_module(library(dict), [dic_lookup/3, dic_get/3]).

:- use_module(engine(runtime_control), [current_prolog_flag/2, prolog_flag/3]).

:- if(defined(optim_comp)).
:- else.
% (see shell_directive/1)
:- use_module(engine(runtime_control), [set_prolog_flag/2, push_prolog_flag/2, pop_prolog_flag/1]).

:- use_module(library(rtchecks/rtchecks_utils), [call_rtc/1]).
:- use_module(library(read_from_string)).
:- endif.

% ---------------------------------------------------------------------------
% Access to toplevel_scope (a 'user' module)

% ('user' modules need to be imported with ensure_loaded/1)
:- ensure_loaded(library(toplevel/toplevel_scope)).
:- import(user, ['$shell_module'/1, '$shell_call'/1]).

% ---------------------------------------------------------------------------

:- use_module(library(system), [file_exists/1]).
:- use_module(engine(runtime_control), [prompt/2]). % TODO: move prompt/2 to some IO related module?
:- use_module(library(operators), [op/3]).

:- multifile exit_hook/0, after_query_hook/0, after_solution_hook/0.

define_flag(prompt_alternatives_no_bindings, [on, off], off).

:- data shell_module/1. % Module where queries are called

:- export(toplevel/1).
toplevel(Args) :-
	get_alias_path,
	%
	'$shell_module'(Module),
	retractall_fact(shell_module(_)), % clean shell_module/1
	asserta_fact(shell_module(Module)),
	%
	retractall_fact(quiet_mode),
	interpret_args(Args, true),
	( quiet_mode -> true ; displayversion ),
	op(900, fy, [(spy), (nospy)]),
	shell_body,
	( '$nodebug_call'(exit_hook), fail ; true ).

:- data quiet_mode/0.

interpret_args([], DefLoad) :- !,
	( DefLoad = true ->
	    load_default
	; true
	).
interpret_args(['-f'|R], _) :- !, % fast start
	interpret_args(R, false).
interpret_args(['-q'|R], DefLoad) :- !, % quiet mode
	set_fact(quiet_mode),
	interpret_args(R, DefLoad).
interpret_args(['-i'|R], DefLoad) :- !,
	'$force_interactive',
	interpret_args(R, DefLoad).
interpret_args(['--version'], _) :- !,
	'$bootversion', % Display Ciao version
	halt.
interpret_args(['-l',File|R], _) :- !,
	( file_exists(File) ->
	    include(File)
	; message(warning, ['File not found (-l option): ', File])
	),
	interpret_args(R, false).
interpret_args(['-u',File|R], DefLoad) :- !,
	use_module(File),
	interpret_args(R, DefLoad).
interpret_args(['-e',Query|R], DefLoad) :- !,
	read_from_atom(Query, Goal),
	'$shell_call'(Goal),
	interpret_args(R, DefLoad).
interpret_args(['-p',Prompt|R], DefLoad) :- !,
	top_prompt(_, Prompt),
	interpret_args(R, DefLoad).
interpret_args(_Args, _) :-
	display(
'Usage: ciaosh [-f] [-q] [-i] [-l <File>] [-u <File>] [-p <Prompt>] [-e <Query>]'),
	nl,
	halt(1).

load_default :-
	RCFile = '~/.ciaorc',
        ( file_exists(RCFile) ->
            include(RCFile)
        ; default_shell_package(Package),
          prolog_flag(quiet, QF, warning),
	  use_package(Package),
          prolog_flag(quiet, _, QF) 
        ).

:- if(defined(optim_comp)).
default_shell_package(default_for_ciaosh).
:- else.
:- use_module(library(compiler/c_itf), [default_shell_package/1]).
:- endif.

% ---------------------------------------------------------------------------

:- use_module(library(errhandle), [error_protect/2]).

:- export('$shell_abort'/0).
'$shell_abort' :-
	message(user, '{ Execution aborted }'),
	shell_body, % Enter toplevel again
	( '$nodebug_call'(exit_hook), fail ; true ),
	% TODO: This is useful for batched execution, but it may
	%   produce strange behaviour as an interactive toplevel (add
	%   a way to clear the error status?)
	halt(1).

shell_body :-
	intercept(error_protect(top_shell_env, fail), % TODO: fail or abort?
	    control_c,
	    do_interrupt_command(0'\n)).

top_shell_env :-
	reset_query_level,
	catch(shell_env(_Vars), go_top, top_shell_env).

shell_env(Vars) :-
	repeat,
	shell_query(Vars, Query),
	('$nodebug_call'(after_query_hook), fail ; true),
	Query == end_of_file,
	!.

:- push_prolog_flag(multi_arity_warnings, off).

:- data top_prompt/1.

:- pop_prolog_flag(multi_arity_warnings).

:- data top_prompt_base/1.

top_prompt_base('?- ').

:- export(top_prompt/2).
% Actually, sets top_prompt_base, but since seen externally, used simpler name
top_prompt(Old, New) :-
	top_prompt_base(Old),
	retract_fact(top_prompt_base(Old)),
	asserta_fact(top_prompt_base(New)).

% Note: up/0 & top/0 checked explicitly

shell_query(Variables, Query) :-
	% TODO: unsafe? what if some thread is still running?
	'$empty_gcdef_bin', % Really get rid of abolished predicates
	debugger_info,
	current_fact(top_prompt(TP)),
	prompt(Prompt, TP),
	(true ; prompt(_, Prompt), fail),
	get_query(Query, Variables, VarNames),
	Query \== up,
	prompt(_, Prompt),
	!,
	( Query == top ->
	    (quiet_mode -> true ; top_nl), throw(go_top)
	; valid_solution(Query, Variables, VarNames) ->
	    (quiet_mode -> true ; top_nl, top_display(yes))
	; (quiet_mode -> true ; top_nl, top_display(no))
	),
	(quiet_mode -> true ; top_nl).
shell_query(_Variables, end_of_file).

debugger_info :-
	get_debugger_state(State),
	arg(1, State, T),
	( T = off, !
	; top_display('{'), top_display(T), top_display('}\n')
	).

get_query(Query, Dict, VarNames) :-
	read_query(RawQuery, Dict, VarNames),
	shell_expand(RawQuery, VarNames, Query).

valid_solution(Query, Variables, VarNames) :-
	( adjust_debugger ; switch_off_debugger, fail ),
	error_protect(call_rtc(shell_call(Query, MoreSols, VarNames)), fail), % TODO: unfold error_protect, fix query level + throw(a) issue
	( switch_off_debugger ; adjust_debugger, fail ),
	('$nodebug_call'(after_solution_hook), fail ; true),
	current_prolog_flag(check_cycles, CyclesFlag),
	( CyclesFlag = on ->
	    compute_solution_cycles(Variables, Solution)
	;
	    compute_solution_nocycles(Variables, Solution)
	),
	display_ok_solution(Solution, Variables, MoreSols).

shell_call(Query, MoreSols, VarNames) :-
	'$metachoice'(BeforeChoice),
	'$shell_call'(srcdbg_spy(Query, _, _, _, _, d(VarNames, []), _)),
	'$metachoice'(AfterChoice),
	( BeforeChoice = AfterChoice ->
	    MoreSols = false
	; MoreSols = true
	).

compute_solution_cycles(Variables, Solution) :-
	del_hidden_vars(Variables, Varqueue, Varqueue_),
	reverse_dict(Varqueue, Varqueue_, RevDict),
	uncycle_eqs(Varqueue, Varqueue_, 0, NewVarIdx, RevDict,
	    Varq_nc, Varq_nc_),
	answer_constraints((Varq_nc, Varq_nc_), (Varq_nc2, Varq_nc2_),
	    Constraints),
	uncycle_constraints(Constraints, NewVarIdx,
	    Constraints_nc_eqs),
	solution_eqs(Varq_nc2, Varq_nc2_, Solution, Constraints_nc_eqs).

compute_solution_nocycles(Variables, Solution) :-
	answer_constraints_nc(Variables, Dict, Constraints),
	solution_vars(Dict, Eqs, []),
	Solution = [Eqs|Constraints].

del_hidden_vars(Var,                         L, L_) :- var(Var), !, L = L_.
del_hidden_vars(dic(Var, [Val|_], Lft, Rgt), L, L_) :-
	del_hidden_vars(Lft, L, L1),
	( Var = "_"||_ ->
% Vars starting with "_" are not printed
	    L1 = L2
	;
	    L1 = [(Var=Val)|L2]
	),
	del_hidden_vars(Rgt, L2, L_).

reverse_dict(Q,             Q_, _) :- Q == Q_, !.
reverse_dict([(Var=Val)|Q], Q_, RevDict) :-
	dic_lookup(RevDict, Val, VarX),
	(var(VarX) -> VarX = Var ; true),
	reverse_dict(Q, Q_, RevDict).

uncycle_eqs(EqL, EqL_, N, Nlast, _RevDict, EqLnc, EqLnc_) :-
	EqL == EqL_, !,
	Nlast = N,
	EqLnc = EqLnc_.
uncycle_eqs([(Var=Val)|EqL], EqL_, N, Nlast, RevDict,
	    [(Var=NewVal)|EqLnc], EqLnc_) :-
	uncycle_val(Val, [], N, N1, RevDict, EqL_, EqL_2, NewVal),
	uncycle_eqs(EqL, EqL_2, N1, Nlast, RevDict, EqLnc, EqLnc_).

uncycle_val(Val, _Seen, N, N1, _RevDict, NewEqs, NewEqs_, NewVal) :-
	var(Val), !,
	N1 = N,
	NewEqs = NewEqs_,
	NewVal = Val.
uncycle_val(Val, _Seen, N, N1, _RevDict, NewEqs, NewEqs_, NewVal) :-
	atomic(Val), !,
	N1 = N,
	NewEqs = NewEqs_,
	NewVal = Val.
uncycle_val(Val, Seen, N, N1, RevDict, NewEqs, NewEqs_, NewVal) :-
	already_seen(Seen, Val), !,
	dic_lookup(RevDict, Val, Var),
	( var(Var) ->
	    new_varname(N, Var),
	    N1 is N+1,
	    NewEqs = [(Var=Val)|NewEqs_]
	;
	    N1 = N,
	    NewEqs_ = NewEqs
	),
	atom_codes(VarName, Var),
	NewVal = '$VAR'(VarName).
uncycle_val(Val, Seen, N, N1, RevDict, NewEqs, NewEqs_, NewVal) :-
	functor(Val,    F, A),
	functor(NewVal, F, A),
	uncycle_val_args(A, Val, [Val|Seen], N, N1, RevDict,
	    NewEqs, NewEqs_, NewVal).

uncycle_val_args(0, _,   _,    N, N,  _,       NewEqs, NewEqs,  _) :- !.
uncycle_val_args(A, Val, Seen, N, N_, RevDict, NewEqs, NewEqs_, NVal) :-
	A1 is A-1,
	arg(A, Val,  ValA),
	arg(A, NVal, NValA),
	uncycle_val(ValA, Seen, N, N1, RevDict, NewEqs, NewEqs1, NValA),
	uncycle_val_args(A1, Val, Seen, N1, N_, RevDict,
	    NewEqs1, NewEqs_, NVal).

already_seen([T|_], Term) :-
	T == Term, !.
already_seen([_|Ts], Term) :-
	already_seen(Ts, Term).

new_varname(N, Var) :-
	number_codes(N, NS),
	Var = "_"||NS.

% ---------------------------------------------------------------------------

:- use_module(library(attrdump), [copy_extract_attr/3, copy_extract_attr_nc/3]).

:- multifile dump_constraints/3. /* For clp[qr] .DCG. */

answer_constraints(Variables, Dict, Constraints) :-
	dump_constraints(Variables, Dict, Constraints), !.
answer_constraints(Variables, Dict, Constraints) :-
	copy_extract_attr(Variables, Dict, Constraints).

answer_constraints_nc(Variables, Dict, Constraints) :-
	dump_constraints(Variables, Dict, Constraints), !.
answer_constraints_nc(Variables, Dict, Constraints) :-
	copy_extract_attr_nc(Variables, Dict, Constraints).

uncycle_constraints(Cs, N, Cs_nc_eqs) :-
	uncycle_val(Cs, [], N, N1, RevDict, Varq, Varq_, Cs_nc),
	uncycle_eqs(Varq, Varq_, N1, _, RevDict, Varqn, []),
	Cs_nc_eqs = [Cs_nc|Varqn].

solution_eqs(EqL, EqL_, SolEqs, SolEqs_) :-
	EqL == EqL_, !,
	SolEqs = SolEqs_.
solution_eqs([(Var=Val)|EqL], EqL_, SolEqs, SolEqs_) :-
	( var(Val) ->
	    atom_codes(AtomVar, Var),
	    Val='$VAR'(AtomVar),
	    SolEqs = SolEqs1
	;
	    SolEqs = [(Var=Val)|SolEqs1]
	),
	solution_eqs(EqL, EqL_, SolEqs1, SolEqs_).

solution_vars(D) --> {var(D)}, !.
solution_vars(dic(Var, [Val|_], L, R)) -->
	solution_vars(L),
	solution_var(Var, Val),
	solution_vars(R).

solution_var([0'_|_], _) --> !. % Do not display vars starting with "_"
solution_var(Var,     Val) --> {var(Val)}, !,
	{atom_codes(AtomVar, Var), Val='$VAR'(AtomVar)}.
solution_var(Var, Val) -->
	[Var = Val].

display_ok_solution(Solution, Variables, MoreSols) :-
	prettyvars(Solution, Variables),
	current_output(StrOut),
	set_output(user_output),
	display_solution(Solution, '', Sep),
	set_output(StrOut),
	ok_solution(Sep, Solution, Variables, MoreSols).

display_solution([], Sep, Sep) :-
	!.
display_solution([C|Cs], Sep0, Sep) :-
	!,
	display_solution(C,  Sep0, Sep1),
	display_solution(Cs, Sep1, Sep).
display_solution(true, Sep, Sep) :-
	!.
display_solution((G1, G2), Sep0, Sep) :-
	!,
	display_solution(G1, Sep0, Sep1),
	display_solution(G2, Sep1, Sep).
display_solution((Var=Val), Sep, ',') :-
	!,
	display(Sep), nl,
	write_string(Var),
	display(' = '),
	write_term(Val, [quoted(true), portrayed(true),
		numbervars(true), priority(699)]).
display_solution(attach_attribute(X, A), Sep, ',') :-
	!,
	display(Sep), nl,
	write(X),
	display(' attributed '),
	write_term(A, [quoted(true), portrayed(true),
		numbervars(true)]).
display_solution(G, Sep, ',') :-
	display(Sep), nl,
	write_term(G, [quoted(true), portrayed(true),
		numbervars(true)]).

ok_solution('', _, _, MoreSols) :-
	current_prolog_flag(prompt_alternatives_no_bindings, Prompt),
	(Prompt = off ; Prompt = on, MoreSols = false),
	!.
ok_solution(Sep, Solution, Variables, MoreSols) :-
	(Sep = '' -> top_nl, top_display('true') ; true),
	top_display(' ? '),
	top_flush,
        top_get(C), ( C = 0'\n -> true ; top_skip(0'\n) ), % ask the user
	( C = 0'y -> true % y(es)
	; C = 0'Y -> true % Y(es)
	; C = 0'\n -> true % end of line
	; C = 0', ->
	    % add another question
	    top_nl,
	    inc_query_level,
	    shell_env(Variables),
	    dec_query_level,
	    display_ok_solution(Solution, Variables, MoreSols)
	; fail % another solution
	).

% ---------------------------------------------------------------------------
% This is alike the one in library(write), except that variable names
% start with "_"
% TODO: do not duplicate code

:- use_module(library(sort), [keysort/2]).

prettyvars(Term, Variables) :-
	collect_vars(Term, Vars0, []),
	keysort(Vars0, Vars),
	pretty_vars(Vars, Variables, 0, _).

collect_vars(Var) -->
	{var(Var)}, !, [Var-[]].
collect_vars([X|Xs]) --> !,
	collect_vars(X),
	collect_vars(Xs).
collect_vars(X) -->
	{functor(X, _, A)},
	collect_vars_(0, A, X).

collect_vars_(A,  A, _) --> !.
collect_vars_(A0, A, X) -->
	{A1 is A0+1},
	{arg(A1, X, X1)},
	collect_vars(X1),
	collect_vars_(A1, A, X).

pretty_vars([], _Variables, N, N).
pretty_vars([X, Y|Xs], Variables, N0, N2):-
	X==Y, !,
	X = ('$VAR'(Name)-[]),
	free_name_var(Name, Variables, N0, N1),
	pretty_vars_(Xs, X, Variables, N1, N2).
pretty_vars(['$VAR'('_')-[]|Xs], Variables, N0, N1):-
	pretty_vars(Xs, Variables, N0, N1).

pretty_vars_([X|Xs], Y, Variables, N0, N1):-
	X==Y, !,
	pretty_vars_(Xs, Y, Variables, N0, N1).
pretty_vars_(Xs, _, Variables, N0, N1) :-
	pretty_vars(Xs, Variables, N0, N1).

free_name_var(Name, Variables, N0, N1) :-
	Letter is N0 mod 26 + 0'A,
	( N0>=26 ->
	    Rest is N0//26,
	    number_codes(Rest, Index)
	; Index = ""
	),
	StrName = [0'_, Letter|Index], 
	\+  dic_get(Variables,StrName,_), !,
	atom_codes(Name, StrName), 
	N1 is N0 + 1.
free_name_var(X, Variables, N0, N2) :-
	N1 is N0 + 1,
	free_name_var(X, Variables, N1, N2).

% ---------------------------------------------------------------------------
	
:- data querylevel/1.

reset_query_level :-
	retractall_fact(querylevel(_)),
	asserta_fact(querylevel(0)),
	set_top_prompt(0).

inc_query_level :-
	retract_fact(querylevel(N)),
	N1 is N+1,
	asserta_fact(querylevel(N1)),
	set_top_prompt(N1).

dec_query_level :-
	retract_fact(querylevel(N)),
	N1 is N-1,
	asserta_fact(querylevel(N1)),
	set_top_prompt(N1).

set_top_prompt(0) :- !,
	retractall_fact(top_prompt(_)),
	top_prompt_base(P),
	asserta_fact(top_prompt(P)).

set_top_prompt(N) :-
	number_codes(N, NS),
	atom_codes(NA, NS),
	top_prompt_base(P),
	atom_concat(NA,  ' ', NS1),
	atom_concat(NS1, P,   TP),
	retractall_fact(top_prompt(_)),
	asserta_fact(top_prompt(TP)).

% ---------------------------------------------------------------------------

:- data '$current version'/1.

:- export(displayversion/0).
displayversion :- % shall use current output
	( '$bootversion',
	    current_fact('$current version'(Msg)),
	    nl, write(Msg), nl,
	    fail
	; true
	).

:- export(version/1).
version(A) :-
	nonvar(A), !,
	assertz_fact('$current version'(A)).
version(_) :- throw(error(instantiation_error, version/1 -1)).

% ---------------------------------------------------------------------------
% Query reader

:- use_module(library(read), [read_term/3]).

read_query(Query, Dict, VarNames) :-
	repeat,
	Opts = [dictionary(Dict), variable_names(VarNames)],
	catch(read_term(user, Query, Opts),
	      error(syntax_error([L0, L1, Msg, ErrorLoc]), _),
	      handle_syntax_error(L0, L1, Msg, ErrorLoc)),
	!,
	Query \== end_of_file.

handle_syntax_error(L0, L1, Msg, ErrorLoc) :-
	display(user_error, '{SYNTAX '),
	message_lns(error, L0, L1, [[](Msg), '\n', [](ErrorLoc), '\n}']),
	fail.

% ---------------------------------------------------------------------------
% Including files (source or packages) in shell

% TODO: move to an included file to the compiler

:- use_module(engine(internals), ['$open'/3]).

:- data new_decl/1.

% (exported)
% TODO: Share the duplicated logic with compiler/c_itf.pl
do_include(Type, File) :-
	absolute_file_name(File, '_opt', '.pl', '.', SourceFile, SourceBase, _),
	include_message(Type, SourceFile),
	'$open'(SourceFile, r, Stream),
	include_st(Type, Stream, SourceBase),
	close(Stream),
	message(user, '}').

include_message(source, SourceFile) :- message(user, ['{Including ', SourceFile]).
include_message(package, SourceFile) :- message(user, ['{Using package ', SourceFile]).

include_st(Type, Stream, Base) :-
	current_fact(shell_module(ShMod)),
	do_read_sentence(Stream, Sentence),
	check_include_decl(Type, Base, Sentence, Rest),
	%
	( member(Sentence2, Rest) 
	; repeat,
	  do_read_sentence(Stream, Sentence2) 
	),
	%read_term(Stream, RawData, [variable_names(VarNames), lines(L0, L1)]),
	( Sentence2 = end_of_file(_,_), !
	; Sentence2 = sentence(RawData, VarNames, _, L0, L1),
	  expand_term(RawData, ShMod, VarNames, Data0),
	  nonvar(Data0),
	  ( Data0 = [_|_] ->
	      member(Data1, Data0)
	  ; Data1 = Data0
	  ),
	  ( Data1 = end_of_file, !
	  ; interpret_data(Data1, L0, L1),
	    fail
	  )
	).

% TODO: share duplicated code with c_itf.pl
do_read_sentence(Stream, Sentence) :-
	Opts = [ variable_names(VarNames),
		 singletons(Singletons),
		 lines(Ln0, Ln1) ],
	read_term(Stream, Data, Opts),
	( Data = end_of_file ->
	    Sentence = end_of_file(Ln0, Ln1)
	; Sentence = sentence(Data, VarNames, Singletons, Ln0, Ln1)
	),
	!.

:- use_module(library(compiler/c_itf), [module_from_base/2]).

% Check that packages contains the right declarations. Nothing is
% required for included source.
check_include_decl(source, _, Sentence, [Sentence]).
check_include_decl(package, Base, Sentence, Sentences) :-
	( Sentence = sentence(Data, _, _, Ln0, Ln1),
	  Data = (:- package(M)) ->
	    Sentences = [],
	    module_from_base(Base, SM),
	    ( SM = M -> % Allow vars in package declarations
	        true
	    ; message_lns(error, Ln0, Ln1, ['Bad package ',M,' in package declaration'])
	    )
	; % Do not consume the sentence, it is not a valid package declaration
          Sentences = [Sentence],
	  sentence_lines(Sentence, Ln0, Ln1),
	  warning_package_missing(Ln0, Ln1)
	).

sentence_lines(sentence(_,_,_,Ln0,Ln1), Ln0, Ln1).
sentence_lines(end_of_file(Ln0,Ln1), Ln0, Ln1).
	
warning_package_missing(L0, L1) :-
	message_lns(warning, L0, L1,
	             ['Source used as package without package declaration']).

interpret_data((?- Goal), _, _) :- !,
	'$shell_call'(Goal), !.
interpret_data((:- Decl), L0, L1) :- !,
	( Decl = multifile(_) ->
	    '$shell_call'(Decl)
	; current_fact(new_decl(Decl)) ->
	    true
	; shell_directive(Decl) ->
	    call(Decl)
	; bad_shell_directive(Decl, L0, L1)
	).
interpret_data(Clause, _, _) :-
	% TODO: needs dynamic_rt:assertz/1 in toplevel_scope.pl
	'$shell_call'(assertz(Clause)).

bad_shell_directive(Decl, L0, L1) :-
	functor(Decl, F, A),
	message_lns(error, L0, L1,
	    [~~(F/A), ' directive not allowed in shell']).

shell_directive(use_module(_)).
shell_directive(use_module(_, _)).
shell_directive(ensure_loaded(_)).
shell_directive(include(_)).
shell_directive(use_package(_)).
shell_directive(set_prolog_flag(_, _)).
shell_directive(push_prolog_flag(_, _)).
shell_directive(pop_prolog_flag(_)).
shell_directive(op(_, _, _)).
shell_directive(new_declaration(_, _)).
shell_directive(new_declaration(_)).
shell_directive(load_compilation_module(_)).
shell_directive(add_sentence_trans(_, _)).
shell_directive(add_term_trans(_, _)).
shell_directive(add_goal_trans(_, _)).
shell_directive(multifile(_)).

% ---------------------------------------------------------------------------

:- export(use_module/1).
use_module(M) :-
	use_module(M, all).

:- export(use_module/2).
use_module(M, Imports) :-
	current_fact(shell_module(Module)),
	use_module(M, Imports, Module).

:- export(ensure_loaded/1).
ensure_loaded([]) :- !.
ensure_loaded([File|Files]) :- !,
	ensure_loaded__2(File),
	ensure_loaded(Files).
ensure_loaded(File) :-
	ensure_loaded__2(File).

ensure_loaded__2(File) :-
	current_fact(shell_module(Module)),
	compiler:ensure_loaded(File, Module).

:- export('.'/2).
[File|Files] :-
	ensure_loaded([File|Files]).

:- export(make_exec/2).
:- redefining(make_exec/2).
make_exec(Files, ExecName) :-
	( Files = [_|_] -> Files2 = Files ; Files2 = [Files] ),
	exemaker:make_exec(Files2, ExecName).

:- export(include/1).
include(File) :- do_include(source, File).

:- export(use_package/1).
use_package([]) :- !.
use_package([F|Fs]) :- !,
	use_package(F),
	use_package(Fs).
use_package(F) :- atom(F), !,
	do_include(package, library(F)).
use_package(F) :- functor(F, _, 1), !,
	do_include(package, F).
use_package(F) :-
	message(error, ['Bad package file ', ~~(F)]).

:- export(new_declaration/2).
new_declaration(S, _) :- new_declaration(S).

:- export(new_declaration/1).
new_declaration(S) :-
	( S = F/A, functor(D, F, A) ->
	    ( current_fact(new_decl(D)) -> true
	    ; asserta_fact(new_decl(D))
	    )
	; message(error, ['Bad predicate specifier ', S,
		    'in new_declaration directive'])
	).

:- export(load_compilation_module/1).
load_compilation_module(File) :-
	this_module(M),
	use_module(File, all, M), % Here for sentence/term expansions
	current_fact(shell_module(ShM)),
	use_module(File, all, ShM). % In toplevel_scope for goal expansions

% ---------------------------------------------------------------------------

:- export(add_sentence_trans/2).
add_sentence_trans(P, Prior) :-
	current_fact(shell_module(ShMod)),
	( translation:add_sentence_trans_and_init(ShMod, P, Prior) ->
	    true
	; message(warning, [add_sentence_trans(P, Prior), ' - declaration failed'])
	).

% ---------------------------------------------------------------------------

:- export(add_term_trans/2).
add_term_trans(P, Prior) :-
	current_fact(shell_module(ShMod)),
	( translation:add_term_trans(ShMod, P, Prior) ->
	    true
	; message(warning, [add_term_trans(P, Prior), ' - declaration failed'])
	).

% ---------------------------------------------------------------------------

:- doc(subsection, "Translation hooks (term and sentence)").

:- use_module(library(compiler/translation),
	    [expand_term/4, add_sentence_trans_and_init/3, add_term_trans/3]).

shell_expand(V, _, Query) :- var(V), !, Query = call(V).
shell_expand((:- Decl), VarNames, Query) :- !,
	current_fact(shell_module(ShMod)),
	expand_term((:- Decl), ShMod, VarNames, Query),
	(Query = true -> true ; true). % unify Query if a var
shell_expand(RawQuery, VarNames, Query) :-
	current_fact(shell_module(ShMod)),
	expand_term(('SHELL':-RawQuery), ShMod, VarNames, Expansion),
	( Expansion = ('SHELL':-Query), !
	; Query = fail,
	  message(error, ['unexpected answer from expansion: ', Expansion])
	).

% ---------------------------------------------------------------------------

:- doc(subsection, "Translation hooks (goal)").

:- use_module(library(goal_trans), [add_goal_trans/3]).

:- export(add_goal_trans/2).
add_goal_trans(P, Prior) :-
	current_fact(shell_module(ShMod)),
	( goal_trans:add_goal_trans(ShMod, P, Prior) ->
	    true
	; message(warning, [add_goal_trans(P, Prior), ' - declaration failed'])
	).

% ---------------------------------------------------------------------------

:- include(library(toplevel/toplevel_debugger)).
