# TCC Test Suite Report for RCC
Generated: May 2026

## Summary
 - **Total**:     138
 - **Passed**:    27
 - **Failed**:    111
 - **Pass Rate**: 19%

## Detailed Results
| Test                                     | Status       | Message                              |
|:-----------------------------------------|:-------------|:-------------------------------------|
| 00_assignment                            | MISMATCH     | Output does not match .expect        |
| 01_comment                               | MISMATCH     | Output does not match .expect        |
| 02_printf                                | MISMATCH     | Output does not match .expect        |
| 03_struct                                | MISMATCH     | Output does not match .expect        |
| 04_for                                   | MISMATCH     | Output does not match .expect        |
| 05_array                                 | MISMATCH     | Output does not match .expect        |
| 06_case                                  | MISMATCH     | Output does not match .expect        |
| 07_function                              | MISMATCH     | Output does not match .expect        |
| 08_while                                 | MISMATCH     | Output does not match .expect        |
| 09_do_while                              | MISMATCH     | Output does not match .expect        |
| 10_pointer                               | COMPILE_FAIL | executable missing                   |
| 11_precedence                            | MISMATCH     | Output does not match .expect        |
| 12_hashdefine                            | MISMATCH     | Output does not match .expect        |
| 13_integer_literals                      | MISMATCH     | Output does not match .expect        |
| 14_if                                    | MISMATCH     | Output does not match .expect        |
| 15_recursion                             | MISMATCH     | Output does not match .expect        |
| 16_nesting                               | MISMATCH     | Output does not match .expect        |
| 17_enum                                  | COMPILE_FAIL | executable missing                   |
| 18_include                               | MISMATCH     | Output does not match .expect        |
| 19_pointer_arithmetic                    | MISMATCH     | Output does not match .expect        |
| 20_pointer_comparison                    | MISMATCH     | Output does not match .expect        |
| 21_char_array                            | MISMATCH     | Output does not match .expect        |
| 22_floating_point                        | COMPILE_FAIL | executable missing                   |
| 23_type_coercion                         | COMPILE_FAIL | executable missing                   |
| 24_math_library                          | COMPILE_FAIL | executable missing                   |
| 25_quicksort                             | MISMATCH     | Output does not match .expect        |
| 26_character_constants                   | MISMATCH     | Output does not match .expect        |
| 27_sizeof                                | MISMATCH     | Output does not match .expect        |
| 28_strings                               | MISMATCH     | Output does not match .expect        |
| 29_array_address                         | MISMATCH     | Output does not match .expect        |
| 30_hanoi                                 | MISMATCH     | Output does not match .expect        |
| 31_args                                  | MISMATCH     | Output does not match .expect        |
| 32_led                                   | MISMATCH     | Output does not match .expect        |
| 33_ternary_op                            | COMPILE_FAIL | executable missing                   |
| 34_array_assignment                      | MISMATCH     | Output does not match .expect        |
| 35_sizeof                                | MISMATCH     | Output does not match .expect        |
| 36_array_initialisers                    | MISMATCH     | Output does not match .expect        |
| 37_sprintf                               | MISMATCH     | Output does not match .expect        |
| 38_multiple_array_index                  | MISMATCH     | Output does not match .expect        |
| 39_typedef                               | MISMATCH     | Output does not match .expect        |
| 40_stdio                                 | MISMATCH     | Output does not match .expect        |
| 41_hashif                                | MISMATCH     | Output does not match .expect        |
| 42_function_pointer                      | COMPILE_FAIL | executable missing                   |
| 43_void_param                            | MISMATCH     | Output does not match .expect        |
| 44_scoped_declarations                   | MISMATCH     | Output does not match .expect        |
| 45_empty_for                             | MISMATCH     | Output does not match .expect        |
| 46_grep                                  | COMPILE_FAIL | executable missing                   |
| 47_switch_return                         | MISMATCH     | Output does not match .expect        |
| 48_nested_break                          | MISMATCH     | Output does not match .expect        |
| 49_bracket_evaluation                    | COMPILE_FAIL | executable missing                   |
| 50_logical_second_arg                    | MISMATCH     | Output does not match .expect        |
| 51_static                                | COMPILE_FAIL | executable missing                   |
| 52_unnamed_enum                          | MISMATCH     | Output does not match .expect        |
| 54_goto                                  | MISMATCH     | Output does not match .expect        |
| 55_lshift_type                           | COMPILE_FAIL | executable missing                   |
| 60_errors_and_warnings                   | SKIP         | Skipped                              |
| 61_integers                              | MISMATCH     | Output does not match .expect        |
| 64_macro_nesting                         | MISMATCH     | Output does not match .expect        |
| 67_macro_concat                          | MISMATCH     | Output does not match .expect        |
| 70_floating_point_literals               | COMPILE_FAIL | executable missing                   |
| 71_macro_empty_arg                       | MISMATCH     | Output does not match .expect        |
| 72_long_long_constant                    | MISMATCH     | Output does not match .expect        |
| 73_arm64                                 | COMPILE_FAIL | executable missing                   |
| 75_array_in_struct_init                  | MISMATCH     | Output does not match .expect        |
| 76_dollars_in_identifiers                | MISMATCH     | Output does not match .expect        |
| 77_push_pop_macro                        | MISMATCH     | Output does not match .expect        |
| 78_vla_label                             | SKIP         | Skipped                              |
| 79_vla_continue                          | SKIP         | Skipped                              |
| 80_flexarray                             | EXEC_FAIL    | non-zero exit                        |
| 81_types                                 | MISMATCH     | Output does not match .expect        |
| 82_attribs_position                      | COMPILE_FAIL | executable missing                   |
| 83_utf8_in_identifiers                   | COMPILE_FAIL | executable missing                   |
| 84_hex-float                             | MISMATCH     | Output does not match .expect        |
| 85_asm-outside-function                  | COMPILE_FAIL | executable missing                   |
| 86_memory-model                          | MISMATCH     | Output does not match .expect        |
| 87_dead_code                             | COMPILE_FAIL | executable missing                   |
| 88_codeopt                               | COMPILE_FAIL | executable missing                   |
| 89_nocode_wanted                         | COMPILE_FAIL | executable missing                   |
| 90_struct-init                           | COMPILE_FAIL | executable missing                   |
| 91_ptr_longlong_arith32                  | MISMATCH     | Output does not match .expect        |
| 92_enum_bitfield                         | MISMATCH     | Output does not match .expect        |
| 93_integer_promotion                     | MISMATCH     | Output does not match .expect        |
| 94_generic                               | MISMATCH     | Output does not match .expect        |
| 95_bitfields                             | COMPILE_FAIL | executable missing                   |
| 95_bitfields_ms                          | COMPILE_FAIL | executable missing                   |
| 96_nodata_wanted                         | SKIP         | Skipped                              |
| 97_utf8_string_literal                   | MISMATCH     | Output does not match .expect        |
| 98_al_ax_extend                          | SKIP         | Skipped                              |
| 99_fastcall                              | SKIP         | Skipped                              |
| 100_c99array-decls                       | MISMATCH     | Output does not match .expect        |
| 101_cleanup                              | COMPILE_FAIL | executable missing                   |
| 102_alignas                              | MISMATCH     | Output does not match .expect        |
| 103_implicit_memmove                     | MISMATCH     | Output does not match .expect        |
| 104_inline                               | COMPILE_FAIL | executable missing                   |
| 105_local_extern                         | MISMATCH     | Output does not match .expect        |
| 106_versym                               | MISMATCH     | Output does not match .expect        |
| 107_stack_safe                           | COMPILE_FAIL | executable missing                   |
| 108_constructor                          | COMPILE_FAIL | executable missing                   |
| 109_float_struct_calling                 | MISMATCH     | Output does not match .expect        |
| 110_average                              | COMPILE_FAIL | executable missing                   |
| 111_conversion                           | COMPILE_FAIL | executable missing                   |
| 112_backtrace                            | SKIP         | Skipped                              |
| 113_btdll                                | SKIP         | Skipped                              |
| 114_bound_signal                         | SKIP         | Skipped                              |
| 115_bound_setjmp                         | SKIP         | Skipped                              |
| 116_bound_setjmp2                        | SKIP         | Skipped                              |
| 117_builtins                             | MISMATCH     | Output does not match .expect        |
| 118_switch                               | MISMATCH     | Output does not match .expect        |
| 119_random_stuff                         | COMPILE_FAIL | executable missing                   |
| 120_alias                                | SKIP         | Skipped                              |
| 121_struct_return                        | MISMATCH     | Output does not match .expect        |
| 122_vla_reuse                            | SKIP         | Skipped                              |
| 123_vla_bug                              | SKIP         | Skipped                              |
| 124_atomic_counter                       | SKIP         | Skipped                              |
| 125_atomic_misc                          | SKIP         | Skipped                              |
| 126_bound_global                         | SKIP         | Skipped                              |
| 127_asm_goto                             | COMPILE_FAIL | rcc returned non-zero                |
| 128_run_atexit                           | COMPILE_FAIL | executable missing                   |
| 129_scopes                               | COMPILE_FAIL | executable missing                   |
| 130_large_argument                       | COMPILE_FAIL | executable missing                   |
| 131_return_struct_in_reg                 | COMPILE_FAIL | executable missing                   |
| 132_bound_test                           | COMPILE_FAIL | executable missing                   |
| 133_old_func                             | COMPILE_FAIL | executable missing                   |
| 134_double_to_signed                     | COMPILE_FAIL | executable missing                   |
| 135_func_arg_struct_compare              | MISMATCH     | Output does not match .expect        |
| 136_atomic_gcc_style                     | SKIP         | Skipped                              |
| 137_funcall_struct_args                  | COMPILE_FAIL | executable missing                   |
| test_bitfields                           | PASS         | exit=0                               |
| test_builtins                            | COMPILE_FAIL | executable missing                   |
| test_elif2                               | PASS         | exit=0                               |
| test_elif_simple                         | PASS         | exit=0                               |
| test_err                                 | PASS         | compile error as expected            |
| test_func                                | PASS         | exit=0                               |
| test_if                                  | PASS         | exit=0                               |
| test_if2                                 | PASS         | exit=0                               |
| test_if3                                 | PASS         | exit=0                               |
| test_if4                                 | PASS         | exit=0                               |
| test_if5                                 | PASS         | exit=0                               |
| test_if6                                 | PASS         | exit=0                               |
| test_if_nested                           | PASS         | exit=0                               |
| test_if_simple                           | PASS         | exit=0                               |
| test_include                             | PASS         | exit=42                              |
| test_include2                            | PASS         | exit=10                              |
| test_loop                                | PASS         | exit=0                               |
| test_macro                               | PASS         | exit=1                               |
| test_minimal                             | PASS         | exit=0                               |
| test_nested_if                           | PASS         | exit=0                               |
| test_ptr                                 | PASS         | exit=0                               |
| test_real                                | PASS         | exit=0                               |
| test_self_include2                       | PASS         | exit=1                               |
| test_signextend                          | COMPILE_FAIL | executable missing                   |
| test_simple                              | PASS         | exit=1                               |
| test_simple2                             | PASS         | exit=1                               |
| test_str                                 | PASS         | exit=0                               |
| test_struct                              | PASS         | exit=0                               |
| test_with_comment                        | PASS         | exit=0                               |
