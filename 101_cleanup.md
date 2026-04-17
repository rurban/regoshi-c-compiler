Fix the tcc 101_cleanup test, which requires
- struct return values,
- full scope-based __cleanup__ support,
- and return/goto/break/continue.

Let me first see what rcc currently produces:

● Bash(make -s && ./rcc tcc_tests/101_cleanup.c -o /tmp/t101 2>/tmp/t101_warn; echo "=== compile exit: $? ===";
      cat /tmp/t101_warn; echo "=== runtime ==="; /tmp/t101;…)
  ⎿  === compile exit: 1 ===
     tcc_tests/101_cleanup.c:324: error: lvalue required as left operand of assignment
          tl = test_cleanup2();

The tricky part is that the loop variable `i` lives in the for-init
scope, not the compound statement block, so ❯ its cleanup doesn't get
triggered when the body closes. I need to emit cleanup for loop
variables when the entire for statement exits, which means I should
add cleanup emission right before restoring the locals after the loop
finishes.

Rather than trying to emit cleanup nodes inline, I'll wrap the for
statement and its cleanup calls in a block node so they're treated as
a single unit in the statement list.

● The immediate error is "lvalue required" for struct return value
assignment. Let me look at how struct assignments are handled:

Now I'm thinking through how break should interact with cleanup—when
breaking out of a loop, I need to clean up variables declared in the
loop body but not the loop's own variables, which get cleaned up
after the loop exits. Looking at the expected output, a break should
trigger cleanup of inner-scope variables before actually breaking.
So break cleans up both the loop body scope (j) and the for-init
scope (i) when it exits. For the normal loop case where it runs to
completion, `j` gets cleaned up at the end of each iteration while `i`
gets cleaned up once after the loop finishes with its final value of
10.

● Let me look at how codegen handles struct assignment and function calls:

  Searched for 2 patterns, read 1 file (ctrl+o to expand)

● This test requires struct return values and full scope-based
  __cleanup__ support. Let me examine the current state of these
  features:

  Searched for 2 patterns, read 1 file (ctrl+o to expand)

● Let me look at the function codegen prologue and how struct-returning functions are handled:

  Searched for 4 patterns, read 2 files (ctrl+o to expand)

● Let me look at how compound_stmt_ex is implemented:

  Searched for 1 pattern, read 1 file (ctrl+o to expand)

● Let me look at how goto and labels are generated in codegen:

With continue, it's similar—j gets cleaned up and the loop restarts,
while i is cleaned up after the loop exits normally. For the switch
case with break, breaking out of the switch still triggers cleanup of
j from the loop body before exiting entirely. I need to trace through
the cleanup order more carefully. When continue executes at i=0, it
cleans up j(400) immediately. Then at i=1, the break statement exits
the switch block, which triggers cleanup of jj(500), and then when
the loop body ends, j(400) gets cleaned up again before the next
iteration. ips to next iteration. For i=1:
1. jj=500 is created
2. break breaks out of switch (but NOT the for loop!)  support. Let me examine the current
3. Falling through after switch, end of for body
4. jj is... wait, break from switch exits the switch. When break exits the switch block { jj=500; break; }, jj

goes out of scope, so jj(500) is cleaned up. Then j(400) is cleaned up at end of loop iteration.

● So: jj is cleaned up by block-scope cleanup when the {jj=500;
break;} block exits (via break from switch). Then j is cleaned up at
end of loop iteration.


The break here breaks OUT OF THE SWITCH, not the for loop. So the for
  loop continues... wait, but there's only for (int i = 0; i < 2;
...). For i=1, after the switch exits, we reach the end of the loop
body and j(400) gets cleaned up. Then i becomes 2 and the loop
terminates. This gives us the sequence: 400 from i=0, then 500 when
jj's block scope ends via the break, then 400 when j's scope ends at
the loop iteration boundary. The crucial point is that break inside
the switch exits only the switch statement itself, not the enclosing
for loop, and block-scope cleanup triggers when that {jj=500;
break;} block exits.

So the solution is: when break exits a block containing cleanup
variables, emit cleanup code for those variables before the break
instruction. This is analogous to how goto cleanup works—the break
can exit multiple nested scopes, so I need to emit cleanup for all
variables in scopes between the current position and the target ✻
(switch scope for break in switch, for-init scope for break in for
loop, etc.).

I'm realizing the pattern: at break/continue, emit cleanup for all
variables in scopes between the current ───── ❯ position and the
control flow target, then emit the break/continue instruction.

Rather than trying to handle all these cases upfront, let me start
with the simpler case—block-scope cleanup at } for non-function
blocks. This should handle most of the test cases: normal block
exits, loop body cleanup on regular iteration, and some switch
scenarios. Then I'll add the break/continue/goto cleanup logic once
I see what the test actually requires.

For the goto cleanup specifically, I'll track each label's scope level
when it's declared, then when a goto is encountered, I can emit
cleanup for any variables in scopes that are being exited. The
tricky part is handling forward gotos where the label hasn't been
seen yet, but I can defer that or use a two-pass approach if needed.
forward gotos, I'll use a deferred mechanism. At the forward goto
site, I'll emit a cleanup placeholder block, then when the label is
parsed, I'll know its scope level. The problem is the cleanup block
was already emitted without knowing the target scope.

I'm considering a two-pass approach where I first collect all label
scope levels, then regenerate goto cleanup calls with correct
information. But since rcc is single-pass, I need another strategy:
generate cleanup based on what's currently in scope at the goto
site, emitting cleanup for all variables in inner scopes. This
works if the goto jumps to an outer scope, but what about jumping
into a deeper scope?

Looking at a concrete example where a goto jumps from an outer scope
into a nested inner scope—if I emit cleanup for all inner scope
variables, I'd be cleaning up variables that haven't even been
declared yet at the jump point, which would be incorrect. I'm
cataloging all the gotos in the test cases to understand their
directionality. Most are backward gotos where the label is already
defined earlier in the source, but there are a few forward gotos
like goto out; where the label appears later in the function. Let me
trace through test2() to confirm the pattern there. I'm realizing I
need to reconsider the scope logic here. When we're doing a forward
goto into a deeper scope level, we don't need cleanup—we're entering
that scope, not exiting it. The variable "plop" at the current scope
level stays in scope, so there's nothing to clean up before jumping
forward.

But when we're jumping backward or to a shallower scope, that's when
we need to clean up variables that are deeper than our target. So
the rule is: for any goto, clean up variables at scopes deeper than
the target scope but not deeper than where we currently are. Testing
this against the examples—jumping forward to a deeper scope needs no
cleanup, jumping backward to a shallower scope needs to clean up
everything in between, and jumping to the same level needs nothing.
I'm tracing through the scope levels in the check() function to
understand where the out: label sits relative to the nested blocks
and their variable declarations. The label appears before the deeply
nested block containing the "last goto out" variable, so a goto from
inside that block would need to unwind multiple scope levels to
reach it. Continuing through the rest of the code structure, I can
see how the out: label gets reused in different scopes—the forward
gotos from earlier blocks jump to the label at function scope, while
the backward goto from within the nested block jumps back to that
same label now positioned above it. This demonstrates how the same
label name resolves differently depending on where the goto
originates and where the label sits in the control flow.

So breaking down all the goto patterns: the first one jumps backward
to a deeper scope without needing cleanup, the next two jump forward
to the outer scope, then there's a backward jump with no cleanup
required, and finally a backward jump that does need to clean up the
innermost variable before transferring control.

For those forward gotos, I won't know the target scope until the label
is actually defined, so I need a strategy. I'm going to use
trampolines—when I encounter a forward goto, I'll emit a jump to a
temporary trampoline label instead of the final destination, then
record what cleanup is needed. Once the actual label is defined, the
trampoline can emit the necessary cleanup code before jumping to the
real target.

The key insight is handling this at the codegen level rather than
during parsing. When I process a goto, I'll check if the label's scope
is already known; if it's a forward reference, I emit a jump to the
trampoline and store the cleanup information. Then when the label
definition is encountered, I generate the trampoline with all the
recorded cleanup calls.

To make this work, I need to embed cleanup information directly in the
AST nodes themselves. For each goto node, I can attach a cleanup
chain that gets populated during parsing based on the current scope
depth. For forward gotos where the target scope isn't yet known, I'll
conservatively store all current inner-scope cleanups, then later
refine this when the label is actually defined. -scope vars at the
goto site

- If the target label is at a deeper scope than expected, we'd over-clean

For this test, all forward gotos (goto out) jump to out: which is at
fn+1. The cleanup would be for all inner-scope vars (scope > fn+1).
This is correct for these cases.

The only risk is if there's a forward goto to a DEEPER scope label
(like jumping into a block that has no cleanup vars). But in practice,
jumping into a block that has already been defined later in the source
is rare and undefined behavior if the block has cleanup vars.

So my approach is: for forward gotos, emit cleanup for all inner-scope
vars (scope > fn scope level), and for backward gotos, use the known
target label scope level. This handles the test cases
correctly—backward gotos to non_plopage at fn+2 need no cleanup,
backward gotos to again at fn+1 need no cleanup, and the "last goto
out" at fn+3 jumping back to fn+1 needs cleanup for that scope. Now
I'm checking the forward goto from the "tata !" block to out: to
verify this logic holds. I need to determine whether a goto is forward
or backward by checking if the label already exists in the table, then
apply the appropriate cleanup strategy—using the function scope level
as a conservative target for forward gotos. Now I'm ready to implement
this: I'll add a scope level field to track variables, maintain global
scope counters for the current and function scopes, and build a label
scope table to distinguish between forward and backward jumps.

Implementing the cleanup logic... Emit cleanup for vars in locals with
`scope_level` > `fn_scope_level`, then the return node. For goto, I need
to look up the label in a label_scopes table—if it's a backward goto,
emit cleanup for vars between the label's scope and current scope; if
forward, emit cleanup between fn_scope and current scope, then record
it for later fix-up. At label definition, register it with the current
scope level. For break and continue, I need to track their target
scope levels and emit cleanup accordingly. I'm adding static variables
to track the break target scope (for-init or switch scope) and
continue target scope (loop body scope). Now I'm looking at how the
for loop currently handles initialization and scope management... I'm
realizing the codegen approach won't work cleanly since it doesn't
have easy access to cleanup information. Instead, I could wrap the for
loop in a block that handles cleanup after the loop exits, though I
need to think through how this integrates with the statement parsing
flow. Building the cleanup block... I see the issue now—when breaking
from inside the loop body, I need to emit cleanup for variables
declared in that scope before the break actually happens, then the
loop-level cleanup for the loop variable happens after the loop exits.
So for the example with j and i, breaking should clean up j first,
then i gets cleaned up when the for loop completes. I'm realizing that
break needs to know what scope it's exiting to—a break in a for loop
exits the loop's scope, while a break in a switch exits the switch's
scope, and they require different cleanup. I need to track the scope
level that break actually exits to, so I can clean up all variables in
scopes deeper than that target scope before emitting the break
instruction. _init_scope

- for_init_scope = fn+1, so clean up vars at scope > fn+1... but that's
  also the for-init scope!

Actually, break exits the entire for loop including the for-init
scope. So it should clean up BOTH loop body vars AND for-init vars!

For -- loop 2 --:
- j (scope = fn+2 for loop body) = 300 → 300 ✓
- i (scope = fn+1 for for-init) = 0 → 0 ✓ (i was re-incremented to 0 when break hit?
  Wait, i starts at 0 and the loop does ++i after body. But break happens in the first
  iteration before ++i. So i=0 when break is hit. ✓)

So break in a for loop should clean up BOTH loop body vars (scope
fn+2) AND for-init vars (scope fn+1). The tricky part is that the
after-for cleanup block would normally handle for-init vars, but since
break exits both scopes immediately, I need to emit the for-init
cleanup right at the break point to match the expected output where 0
appears immediately after 300, not later. I'm verifying the cleanup
behavior across different loop scenarios—when there's no break or
continue, j gets cleaned up at each iteration while i waits until the
after-for block, and with continue, the same pattern holds since
continue doesn't exit the for-init scope.

Now looking at the switch-break case: when `i=0`, continue triggers `j`'s
cleanup and restarts; when `i=1`, the break from the switch only exits
that scope to clean up `jj`, then `j` gets cleaned up at the loop body
exit, but `i` itself has no cleanup function so it never gets cleaned up
even after the loop ends. loop 3 is for `(int i = 0; ...)` without
`__attribute__((cleanup(cl)))`. So no cleanup of `i`.

Outputs:
- i=0: continue → 400 (cleanup j)
- i=1: break from switch → 500 (cleanup jj) → end of loop body → 400 (cleanup j)
- after break printed

Expected: 400 500 400 after break ✓

Now for the break from switch: what's `break_scope_level` when inside a
switch? The switch is inside the for loop body (scope fn+2), and the
switch statement itself creates no new scope. The default: { jj=500;
break; } block creates scope fn+3, so break should exit to scope fn+2
(after the switch statement).

So `break_scope_level` for a switch equals the scope of the switch
statement itself (fn+2 in this case, which is the for loop body
scope). When breaking from scope fn+3, I clean up variables where
`break_scope_level + 1 ≤ scope ≤ current_scope,`, which gives me `jj` at
fn+3 to clean up (500), then the break exits the switch and continues
in the for loop body where j(400) gets cleaned up when that block
exits normally.

For continue from scope fn+2, the `continue_scope_level` is fn+1 (the
for-init scope where we continue to increment i).

I need to figure out how to track `break_scope_level` and
`continue_scope_level` during parsing, and I'm realizing I need to be
careful about what "scope level" means for break/continue
targets—particularly since the for loop doesn't use a compound_stmt
for its scope the way I initially thought.

Looking at how the parser handles for loops: the for-init declarations
get the current scope level (same as the scope containing the for
statement), while the loop body is a compound_stmt that increments the
scope level. So when a break or continue happens inside a for loop, I
need to clean up variables up to the scope level of the for statement
itself, not the for-init scope. After the for loop exits—whether
through break or naturally—the after-for cleanup block handles the
for-init vars. So I need to set up the scope levels correctly:
for-init vars live at the current scope level, while continue and
break both clean up at the next level deeper (the loop body scope).
For switch statements nested in loops, break needs to target the
switch's own scope level, though switches don't have continue to worry
about.

Looking at the specific case with the switch inside the for loop, when
 break is hit from within a case block, it should clean up only the
 variables declared in that case scope, leaving the loop body
 variables intact. And when continue is executed, it needs to clean up
 the loop body scope before jumping back to the loop condition.

The key insight is that I need to track separate break scope levels
for each breakable construct—one for the innermost loop and another
for any nested switch statement—so the cleanup happens at the right
nesting level.

I'm implementing this with a stack-based approach using saved/restored
scope values. For each for loop and switch statement, I'll save the
current break and continue cleanup scopes, set them to the appropriate
level for that construct, parse the body, then restore the previous
values.

Setting up the scope tracking... Writing helper functions...

Since locals is prepended with each new variable, it's already in LIFO
order, so I can iterate through it directly to emit cleanups in the
right sequence.

Now I'm reconsidering the cleanup logic for return statements. The
issue is whether block-exit cleanup already handles inner scopes,
which could lead to double-cleaning if I'm not careful about which
scope levels I'm targeting.

Let me trace through an example: when a return happens inside an inner
block, I need to clean up variables declared in that inner block and
any deeper scopes, but I should avoid re-cleaning variables at the
function body level since those get handled by the function epilogue.
The key is making sure my scope comparison logic correctly
distinguishes between variables that need immediate cleanup on return
versus those that belong to outer scopes. Now I'm verifying the scope
levels: `fn_scope_level` is the scope level of the function body block
itself (after incrementing in `compound_stmt_ex`), so for a top-level
function it's 1, and variables declared in the function body are at
scope 1. When we hit a return statement in an inner block at scope 2,
we emit cleanup for variables with scope greater than `fn_scope_level`
(1) and up to the current scope (2), which correctly captures only the
inner block variables while leaving function body variables for the
epilogue to handle.

Let me trace through nested returns to make sure this works correctly.

Examining a case with nested blocks and early returns...

When the inner block closes normally, its cleanup runs for variables
in that scope, then they're removed from the locals list. At the
return statement in the outer scope, only variables from the outer
scope remain, so the epilogue handles them without any double-cleanup.
Now I'm thinking through how this applies to loop constructs,
particularly the for loop case where there's cleanup needed both at
loop exit and after the loop body. epilogue. But wait, when the return
happens after the loop instead of inside it, the after-for cleanup
block runs first and removes i from the locals chain, so the epilogue
won't double-clean it since i is no longer tracked in the function's
locals at that point.

Let me trace through the execution: before the for loop, locals
contains just the outer variable a; once we enter the for-init, i gets
added; the loop body declares j which gets added; when the inner block
closes, j gets cleaned up and removed; then after the for statement
completes, locals = saved_locals restores it back to just a,
effectively removing i; the after-for cleanup block then emits cleanup
for i; and finally when the function ends, the epilogue only sees a in
the locals chain, so it cleans up a without touching i.

Now I'm checking what happens if the return statement is inside the
 loop body instead—at that point locals would be [j, i, a], and the
 return cleanup would only target variables at scope > 1, which means
 just j gets cleaned up by the return, while i and a remain for the
 epilogue to handle.

The key insight is that `fn->locals` gets captured during parsing, not
execution. When `compound_stmt_ex` finishes parsing the entire function
body, it captures the final locals list at the end of the closing
brace, after all inner scopes have been unwound and locals has been
restored to just the function-scope variables. So even if a return
happens mid-function, the parser still processes the entire body and
correctly captures which variables belong at function scope.

Now I'm tracing through the parse-time scope management: the function
body opens a new scope level, variables like a get added at that
level, then when the for loop is parsed, its init declares i at the
same scope, and the loop body opens another nested scope where j is
declared. When the return statement is encountered inside that nested
block, cleanup is emitted for variables in that inner scope before the
return, and then the block exit handles cleanup for j as well.

After the for loop completes, the scope pops back and i gets cleaned
up in the after-for block, while a remains in the function's locals
for epilogue cleanup. At runtime, the return statement triggers
cleanup for j, then jumps to the epilogue which cleans up a, but i is
correctly excluded from the function's locals since it was scoped to
the for loop.

Wait, I'm seeing the real issue now: `i` is allocated at scope 1 (the
function body scope) during for-init, but then removed from locals
when the for loop ends. This means `i` isn't in `fn->locals` for epilogue
cleanup, and the return cleanup only handles scope > 1, so `i` never
gets cleaned up at all when a return happens inside the loop.

Let me trace through the test case to see if this actually causes a
problem with any cleanup variables in the for-init.

Looking at the code, there's no return statement inside the loop, so
the after-for cleanup block properly handles the for-init variable i.
The block-scoped variable `j` gets cleaned up at each iteration's end.
This edge case doesn't affect the test.

Now I'm thinking about the scope level assignment for for-init
 variables—they're getting the same scope level as the function body,
 which creates a problem with how block-exit cleanup works.

Actually, wait. When the function body block exits, the saved locals
have already been restored, so for-init variables are no longer in the
locals list. The block-exit cleanup iterates only over what's
currently in locals, so those variables won't be emitted again. And
the function body compound statement doesn't do block-exit cleanup
anyway since it's handled by the epilogue.

Let me trace through what happens with a compound statement inside the
for body that declares its own variables.

The for body is a compound statement with out_locals set to NULL,
meaning it does perform block-exit cleanup when it closes. So
variables declared inside get cleaned up properly. After the for loop
ends, the for-init variable gets removed from locals and cleaned up by
the after-for block.

Alright, the logic checks out. Now I need to implement this. I'm
adding a scope level field to track variable scope depth, introducing
static variables to track the current scope and function scope levels,
and setting up a label scope table for managing break and continue
cleanup.

The implementation requires modifying several key functions: updating
new_var to record scope levels, reworking compound_stmt_ex to handle
scope tracking and cleanup, updating statement parsing for
return/goto/break/continue to emit proper cleanup, and adjusting loop
and switch parsing to track break/continue scopes. I'll also need
helper functions to emit cleanup calls for specific scope ranges.

For code generation, I'm adding a return pointer slot definition,
updating the function prologue to handle struct returns, modifying
return statement codegen for struct values, and handling struct
assignment when the right side is a function call.

This is substantial work. Let me start with the struct return handling
in codegen...

Actually, I should verify the cleanup approach won't break existing
tests first. For 03_struct, the fred variable lives at function body
scope level, and the epilogue will handle its cleanup since function
body blocks don't emit their own cleanup—only inner blocks do. That
should work fine.

Let me check the INCR_GI7 case...

Checking the macro expansion...

The i variable is declared inside an inner block with a higher scope
level, so when that block exits, its cleanup fires and increments the
global counter. With 65536 such blocks, the global should increment by
65536 as expected.

Now I'm verifying whether removing cleanup from the codegen epilogue
and relying on parser-generated AST cleanup would break `03_struct`.
Since fred is at function body scope level and the epilogue still
handles variables at that level, it should be fine. Let me trace
through the actual test case to confirm.

Looking at how the cleanup functions work in the test files...

when str is returned, the pointer value gets captured before the
cleanup runs, so even though check2 modifies the str variable
afterward, the return value is already determined. I need to trace
through the execution order carefully to understand what actually gets
printed. return str` in test():
- Return cleanup: str is at `fn_scope_level`, NOT inner scope. So no
  cleanup by return cleanup.
- Actually wait: str IS at `fn_scope_level` (the function body scope).
Return cleanup handles `scope > fn_scope_level`. So str (scope =
fn_scope_level) is NOT cleaned up by return cleanup.
- The value of str is moved to rax.
- Jump to .L.return
- Epilogue: cleanup str (calls check2: prints str, sets str="fail")
- Return rax (str's original value) with push/pop rax wrapper ✓

Wait but with the push/pop rax, the return value is preserved even if
cleanup modifies str. ✓

Expected: check2 prints str (the original value), then caller prints
test()'s return value (also original). ✓

Now for `test_ret`, when I return the result of calling
`test_ret_subcall(that)`, the subcall executes first and prints its
message, then the epilogue cleanup runs check2 on that before the
function returns.

For main, when i is returned after being modified, the cleanup
 function check runs and modifies i to -1, but the return value was
 already captured before cleanup, so main returns the original value
 of i at the time of the return statement. to understand when the
 cleanup for "tata !" happens—it's being cleaned up when exiting the
 block that contains it, which aligns with my goto implementation
 where cleanup calls are emitted at various jump points within
 check().

Now I'm ready to write the actual implementation code.

Wait, I need to reconsider the macro structure—INCR_GI7 creates 65536
sequential blocks, not nested ones, so each variable declaration sits
at roughly the same scope level rather than increasing nesting depth.

Now I'm ready to implement this. I'll modify rcc.h first to add the
necessary struct definitions, then update parser.c and codegen.c. For
the callee side of struct returns, I need to detect when a function
returns a struct, save the return pointer from the first parameter
register to a reserved slot, and then when returning, copy the struct
data to that address instead of trying to fit it in a register.

On the caller side, when assigning the result of a struct-returning
function call, I'll pass the destination address as a hidden first
argument in the appropriate register, then the actual function
arguments follow in the subsequent registers. The key insight is that
I need to handle the hidden pointer separately from the regular
argument passing logic.

I'm considering a few approaches: I could refactor the FUNCALL
generation to accept a hidden pointer parameter, but that's invasive.
Instead, I could set a static flag before generating the FUNCALL node
to tell it to start argument placement from the second register
onward, since the first register is already occupied by the hidden
pointer address.

Setting up the call by pre-loading the destination address into rdi,
then using a flag to offset the argument register indices...

Checking for register conflicts when freeing dst after the move to
rdi—since the argument generation happens afterward and allocates
fresh registers, there shouldn't be any collision as long as the freed
register gets reused for argument computation rather than interfering
with rdi.

But there's a potential issue with the hidden pointer approach: if
argument generation clobbers rdi while computing values into scratch
registers before moving them to their final positions, that would
corrupt the hidden pointer we just set up. I need to make sure
argument computation doesn't touch rdi, or save and restore it around
that phase. Hmm, but looking at the gen function: it never emits mov
rdi, ... directly (it uses reg(r, ...) which maps to r10, r11, rbx,
r12, etc., not rdi or rsi). The arg setup happens AFTER all arg values
are computed. So rdi is only set at the arg-setup phase.

Wait, but in the funcall gen code (lines 536-554), for arg i=0 (which
we're skipping for struct return), the code sets argreg64[0] = rdi. If
we use funcall_has_hidden_ptr, we start at i=1, so argreg64[1] = rsi,
argreg64[2] = rdx, etc. And rdi is already set to dst (hidden ptr).

But there's still a problem: in step 1 (save scratch regs), if
used_regs includes r10 or r11, we save them. But r10 and r11 aren't
rdi/rsi, so saving scratch regs doesn't touch rdi. During arg
computation in step 2, we might allocate r10 or r11 to compute arg
values, then move those values to argreg64[1], argreg64[2], etc.—the
computed values flow through r10/r11 briefly, not through rdi, so rdi
stays intact. The spill operations for saving scratch regs use fixed
offsets, not rdi, so they don't interfere either. I think this
approach works.

Now I need to handle the platform differences: on Linux the hidden ptr
goes in rdi, on Windows it goes in rcx. I'm writing conditional code
to emit the right mov instruction for each platform, and then I need
to handle the function pointer setup similarly.

I also need to think through struct-returning functions in different
contexts—when they're called as statements where the result is
discarded versus when they're nested as arguments to other functions.
For the test cases I'm working with, I don't think I'll encounter
struct-returning calls, so I can focus on the main assignment case for
now.

Looking at the test code, all the struct-returning calls happen in
assignment contexts like tl = test_cleanup2(). The check() function
that gets called as a cleanup attribute takes an int *, not a struct,
so that's not relevant here. Since struct returns only appear in
assignments in this test, I can implement just that pattern.

For now, I'll handle the case where struct-returning functions are
called standalone by letting them crash or emitting a warning—this
test doesn't need it. Now I need to think through the cleanup timing:
functions like test_cleanup2 have a local variable n with a cleanup
attribute at function scope level. The return cleanup only handles
scopes deeper than function scope, so n gets cleaned up in the
epilogue, which is exactly when the expected output should print.

With struct return via hidden pointer, the local struct gets copied to
the caller's memory before the cleanup runs, so the caller sees the
original values while the cleanup modifies the local copy afterward.

Now I'm ready to write the actual code. I need to track scope levels
for variables so I can properly handle cleanup when exiting
blocks—I'll add a scope_level field to track nesting depth and
introduce some static variables to manage scope tracking during
parsing.

Adding scope tracking infrastructure... I'm creating a helper function
that constructs a cleanup call node for variables with destructors,
wrapping the variable address and function name into an expression
statement. I'm second-guessing whether I need to call add_type on the
address node separately, but realizing that add_type recursively
processes all child nodes through its internal function, so calling it
on the function call node should handle the entire tree.

Now I'm verifying the type inference logic—new_var_node doesn't set
 the type directly, but add_type looks up the variable's type from its
 declaration, and for address operations it should set the node type
 to a pointer to the variable's type. This should work correctly as
 long as the variable was properly declared with a type. purposes. Now
 I'm thinking through a helper function that emits cleanup calls for
 variables within a specific scope range, iterating through the locals
 list in reverse declaration order and building a linked list of
 cleanup nodes to append. Then I need to modify the compound statement
 handler to use this cleanup logic.

Modifying the block handler...

I'm realizing the order matters here—I need to emit the cleanup code
while locals still points to the block's variables, before I restore
the saved locals. And `current_scope_level` needs to stay at the block's
level during cleanup emission, then decrement after. Let me adjust the
sequence so cleanup happens first, then I restore the scope state.

For the function body block where we're not emitting block-exit
cleanup, that's fine since the epilogue handles it. Now I'm moving on
to handle return statements...

Modifying return statement parsing...

Still writing return logic... Building the cleanup chain...

I'm verifying that `emit_cleanup_range` constructs a linked list of
cleanup nodes correctly, then attaches the return node to the tail of
that chain and wraps everything in a block node so the cleanup
statements execute before the return.

Now I'm modifying the goto statement handler to emit cleanup code for
any scopes being exited—I need to look up the target label's scope
level and generate cleanup calls for variables in scopes deeper than
that. I'm verifying the cleanup logic for different goto scenarios.
For forward gotos, the range correctly captures all inner-scope
variables that need cleanup. For backward gotos to outer scopes, the
cleanup range becomes empty since we're asking for variables at a
scope level deeper than the target, which is correct—no cleanup needed
when jumping outward. Let me trace through the backward goto case from
the innermost block to confirm the logic holds. Checking the backward
goto to again at the same scope level — since both the goto and its
target label are at fn_scope+1, there's no cleanup needed between
them.

Now I'm working on how to register label scopes when they're parsed,
storing each label's name and the scope level where it appears so I
can look them up later when processing gotos.

For break and continue statements, I need to emit cleanup code for the
range between where the break/continue happens and the target scope,
then attach the actual break or continue node after that cleanup.

Now I'm handling the for loop statement, which needs to track break
and continue scopes separately and manage cleanup after the loop
completes. Finishing the cleanup logic for variables declared in the
for-init block—I need to identify which variables were added during
that parsing phase and emit cleanup code for those that have
destructors, then restore the saved variable and typedef tables before
returning the loop node, wrapping it in a block if cleanup is needed.

Now I'm realizing the scope level approach won't work since for-init
variables sit at the same scope as outer declarations, so I need to
track variable positions in the linked list instead and create a
helper function that walks the chain between two pointers to find and
clean up any variables with destructors.

Writing the cleanup helper... I need to store the locals pointer at
label definition time so I can use it for cleanup during goto
statements. For backward gotos, I can directly reference that saved
pointer to find which variables need cleanup. For forward gotos where
the label hasn't been defined yet, I'll need to use the function
body's locals as a conservative estimate, which I can capture at parse
time.

Now I'm realizing there's a subtlety with backward gotos to labels in
deeper scopes—the locals list at the time the label was declared
already includes variables from those inner scopes, so the cleanup
logic needs to account for that correctly. So locals_at_label includes
"plop" emit_cleanup_between(locals, locals_at_label, tok) would find
vars in locals before locals_at_label At the time of goto non_plopage,
locals = [... "plop" ...] locals_at_label = the locals list when
non_plopage was defined

When the non_plopage: label is parsed, we're inside the innermost
compound statement that contains only the printf call with no variable
declarations, so locals at that point still reflects the outer scope
variables. I'm verifying the logic with a pointer diagram: since
locals and locals_at_label both point to the same head of the list,
the cleanup loop never executes, which is correct. Now I'm checking
the backward goto case from cascade3's scope after the out label is
defined. I need to trace through what happens when we hit the again:
label after block 2 exits via goto. At that point, the locals stack is
back to [chk, "wololo"] since block 2's variables have been cleaned
up. Now I'm working through the cascade blocks—when we declare
cascade1, it gets added to locals, and then the nested block saves
that state before cascade2 is declared.

Continuing deeper into the nesting, cascade3 gets declared in the
innermost block, and when the goto executes from there, the locals
include all three cascade variables plus the outer scope variables.
But once we unwind back through each block closure, those cascade
variables get cleaned up, so by the time we reach the out: label at
parse time, locals are back to just [chk, "wololo"], which becomes the
`locals_at_label` for that label.

Now when processing the goto out statement inside cascade3's block, I
need to figure out what the target locals should be. The label hasn't
been encountered yet during parsing—it's a forward goto—so I can't
look it up in the label table. Instead, I fall back to using the
function body's entry locals as a conservative estimate of what
variables should be available at the jump target.

For the `check()` function specifically, when the function body starts,
the locals list contains just the parameter j. As the body is parsed
and variables are declared, more locals get added, but the saved
baseline—what I'd use for forward gotos—is just [j].

Now I'm seeing the issue: when emitting cleanup for a forward goto
like goto out, I'm comparing against fn_body_locals = [j], which means
I'd clean up everything added after that point. But that includes
variables like "wololo" that have cleanup functions and shouldn't be
cleaned up at the goto site—they should only be cleaned up at the
actual function return. The problem is that `fn_body_locals` isn't
capturing the right scope boundary for what needs cleanup at a goto
versus what needs cleanup at function exit.

Since I don't know the target label's locals yet with forward gotos, I
need a different strategy. Instead of trying to figure out cleanup at
parse time, I could emit a synthetic node that captures the current
locals state and defer the actual cleanup emission until I know where
the target label is.

--------------

I have the likely root cause for the compile failure: struct
assignment currently tries to take the address of a function-call
result.

I’ve narrowed the implementation to three focused changes:
- parser-side cleanup target tracking,
- function-level resolution for goto/return,
- and minimal struct-return ABI support for the exact failing
  call/return pattern in 101_cleanup.

Thinking: Considering label management

When defining labels later on, I can record local variables and patch
any unresolved gotos to point to cleanup_end, which would be
locals_at_label. I'll also consider maintaining a linked list of
unresolved gotos for each label or globally. That sounds nice! Plus, I
may need to wrap the cleanup for goto, break, or continue statements
if I know what cleanup_end is beforehand. It's all about keeping
everything organized!

I’m switching the control-flow cleanup emission to codegen for
return/goto/break/continue. That avoids the return-value ordering bug
from parser-side wrapping and lets me resolve forward goto targets
once labels are seen.

in codegen the removed peephole pattern 4 turns any mov REG, IMM; mov
[mem], REG into mov byte ptr [mem], IMM just because the immediate
fits in a byte, ignoring the destination width. That’s corrupting int
struct fields and cleanup writes. Fix that narrowing peephole by checking the
the destination width also.

The remaining issues split into four concrete compiler bugs:
1. scratch spill slots overlap real locals,
2. statement-expr result selection is wrong when cleanup code is appended,
3. variadic/default promotions for float are missing,
4. long double calling/return is not implemented correctly.
