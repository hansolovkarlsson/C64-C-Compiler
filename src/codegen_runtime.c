/*
 * codegen_runtime.c - the fixed 6502 runtime library that every
 * compiled program links against, plus the machinery for string
 * literals.
 *
 * ===================================================================
 * WHY A RUNTIME LIBRARY AT ALL
 * ===================================================================
 * The 6502 has no multiply, divide, or 16-bit compare instructions -
 * only 8-bit add-with-carry and subtract-with-borrow. Since cc64's
 * `int` is 16 bits, almost every arithmetic and comparison operator
 * needs more than one or two instructions to implement correctly, and
 * several (multiply, divide, signed comparison) need a genuine
 * multi-instruction algorithm. Rather than inline that algorithm every
 * time the user writes `a * b` in their program, cc64 emits each
 * algorithm ONCE, as a callable subroutine (e.g. __rt_mul16), and
 * every `*` operator anywhere in the user's program just does
 * `JSR __rt_mul16`. This is exactly what a real compiler's runtime
 * library does, just at a much smaller scale.
 *
 * emit_zp_equates() and emit_runtime() are called once per compiled
 * program, early, before any user code is generated (see main.c) -
 * every routine defined here is unconditionally included whether the
 * user's program uses it or not. A production compiler would only
 * link in the routines actually used (this is "dead code elimination"
 * or, applied to a whole library, roughly what a linker's garbage
 * collection does); cc64 skips that for simplicity, at the cost of a
 * little unused code in small programs.
 *
 * ===================================================================
 * THE CALLING CONVENTION THESE ROUTINES SHARE
 * ===================================================================
 * Every routine below reads its input from, and writes its result
 * into, one of the fixed "registers" __zpR / __zpR2 (see cc64.h's
 * architecture note, and the zero-page layout comment inside
 * emit_zp_equates() below) - never via a parameter passed on a stack.
 * The convention, used consistently everywhere in this compiler, is:
 * __zpR2 holds the LEFT operand, __zpR holds the RIGHT operand, and
 * the result ends up in __zpR. So `JSR __rt_mul16` computes
 * __zpR2 * __zpR and leaves the answer in __zpR - the caller (in
 * codegen_expr.c) is responsible for getting both operands into the
 * right registers first, which is what the push/pop routines below
 * (__rt_push / __rt_pop2) exist to make possible for nested
 * expressions like `a * (b + c)`.
 *
 * ===================================================================
 * WHY ZERO PAGE IS SO CAREFULLY RESTRICTED HERE
 * ===================================================================
 * See the long comment inside emit_zp_equates() below for the full
 * story (it's worth reading - it's a real bug this compiler shipped
 * once, from a wrong assumption about which memory was "free"), but
 * the short version: on a real C64, the operating system (the KERNAL
 * and BASIC ROMs) actively use most of zero page for its own
 * bookkeeping, even while your program is running. Only a handful of
 * zero-page bytes are genuinely unclaimed, and this compiler now uses
 * ONLY those - everything else needed lives in ordinary (non-zero-
 * page) RAM instead, which is always safe since a running program has
 * that memory entirely to itself.
 */

#include "cc64.h"

void emit_zp_equates(void) {
    emit("; ---- zero page pseudo-registers ---------------------------------");
    emit("; Only $02/$03 and $FB-$FE are used here. Per the KERNAL's own");
    emit("; memory map, those are the ONLY zero-page bytes confirmed genuinely");
    emit("; free: $F3/$F4 is the current-line-in-color-RAM pointer (touched by");
    emit("; CHROUT itself when it colors a printed character) and $F5/$F6 is the");
    emit("; keyboard-matrix-to-PETSCII conversion table pointer (touched by the");
    emit("; keyboard scan on every background IRQ, ~60x/sec) - both looked like");
    emit("; safe 'adjacent' bytes but are not; this bit us for real once already.");
    emit("__zpAP = $02      ; effective-address pointer (2 bytes) - needs zero");
    emit("                  ; page because it's used with (zp),Y indirection");
    emit("__zpR  = $FB      ; primary register / return value (2 bytes)");
    emit("__zpR2 = $FD      ; secondary operand register (2 bytes)");
    emit("__CHROUT = $FFD2");
    emit(" ");
}

void emit_runtime(void) {
    emit("; ---- runtime scratch storage (plain RAM, NOT zero page - only __zpAP");
    emit("; needs (zp),Y indirection; these are always accessed directly) -------");
    emit("__rt_spidx:"); /* operand-stack index (0-255; stack holds 128 slots) */
    emit("    .byte 0");
    emit("__zpAP2:"); /* saved-AP scratch across RHS evaluation of assignments */
    emit("    .fill 2, 0");
    emit("__zpT1:"); /* runtime scratch (sign flags in __rt_sdivmod16) */
    emit("    .fill 2, 0");
    emit("__zpT0:"); /* runtime scratch (division remainder, multiply accumulator) */
    emit("    .fill 2, 0");
    emit(" ");
    emit("; ---- runtime library -------------------------------------------");
    /* The operand stack: how nested expressions like a*(b+c) work.
     * Evaluating a binary operator needs BOTH operands sitting in
     * registers at once, but there are only two 16-bit "registers"
     * (__zpR/__zpR2) and evaluating the right-hand operand can itself
     * require using them (e.g. the "c" in b+c). The fix is the oldest
     * trick for this: push the left operand's value onto a stack
     * before computing the right operand, then pop it back out right
     * before combining them. See gen_expr_to_R()'s N_BINOP case in
     * codegen_expr.c for the push/.../pop pattern this makes possible.
     *
     * This is a *software* stack cc64 manages itself (the 6502's own
     * hardware stack, page 1, is used only for JSR/RTS return
     * addresses - mixing user data into it would be asking for
     * trouble). It's indexed with plain absolute,X addressing rather
     * than a zero-page pointer, specifically so it doesn't need to
     * claim any more of the scarce, contested zero page (see the
     * comment in emit_zp_equates() below). X is an ordinary CPU
     * register, not memory, so this costs nothing extra in zero page -
     * just the one __rt_spidx byte (ordinary RAM) to remember the
     * current stack depth between calls. */
    emit("__rt_push:"); /* push __zpR onto the operand stack (128 16-bit slots) */
    emit("    LDX __rt_spidx");
    emit("    LDA __zpR");
    emit("    STA __rt_stack,X");
    emit("    LDA __zpR+1");
    emit("    STA __rt_stack+1,X");
    emit("    INX");
    emit("    INX");
    emit("    STX __rt_spidx");
    emit("    RTS");
    emit(" ");
    emit("__rt_pop2:"); /* pop the top of the operand stack into __zpR2 */
    emit("    LDX __rt_spidx");
    emit("    DEX");
    emit("    DEX");
    emit("    LDA __rt_stack,X");
    emit("    STA __zpR2");
    emit("    LDA __rt_stack+1,X");
    emit("    STA __zpR2+1");
    emit("    STX __rt_spidx");
    emit("    RTS");
    emit(" ");
    /* 16x16->16 unsigned multiply, shift-and-add (the standard "grade
     * school" long multiplication algorithm in binary): shift the
     * multiplier (__zpR) right one bit at a time; whenever the bit
     * shifted out is 1, add the multiplicand (__zpR2), which itself
     * gets shifted left one bit each iteration so it's always aligned
     * to the right place value. 16 iterations (one per bit) with the
     * loop counter in X. This computes the correct low 16 bits of the
     * product regardless of whether the *true* mathematical inputs
     * were meant as signed or unsigned - two's complement arithmetic
     * means signed multiplication's low bits come out identical to
     * unsigned, so unlike division there's no separate signed
     * multiply routine needed. */
    emit("__rt_mul16:"); /* __zpR2 * __zpR -> __zpR (16x16->16, truncated) */
    emit("    LDA #0");
    emit("    STA __zpT0");
    emit("    STA __zpT0+1");
    emit("    LDX #16");
    emit("__rt_mul16_loop:");
    emit("    LSR __zpR+1");
    emit("    ROR __zpR");
    emit("    BCC __rt_mul16_noadd");
    emit("    CLC");
    emit("    LDA __zpT0");
    emit("    ADC __zpR2");
    emit("    STA __zpT0");
    emit("    LDA __zpT0+1");
    emit("    ADC __zpR2+1");
    emit("    STA __zpT0+1");
    emit("__rt_mul16_noadd:");
    emit("    ASL __zpR2");
    emit("    ROL __zpR2+1");
    emit("    DEX");
    emit("    BNE __rt_mul16_loop");
    emit("    LDA __zpT0");
    emit("    STA __zpR");
    emit("    LDA __zpT0+1");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    /* Unsigned 16-bit restoring division (dividend __zpR2, divisor
     * __zpR), the standard bit-at-a-time long-division algorithm: shift
     * the dividend left one bit into a running remainder (__zpT0); if
     * the remainder is now >= the divisor, subtract the divisor back
     * out and record a 1 bit in the quotient (reusing __zpR2, whose
     * bits have already been shifted out and are free to reuse - the
     * `INC __zpR2` is what sets that quotient bit, relying on the
     * ASL just above having left its low bit at 0). After 16
     * iterations, __zpR2 holds the quotient and __zpT0 the remainder.
     * This is the *unsigned* primitive that __rt_sdivmod16 below
     * builds signed division and modulo on top of. */
    emit("__rt_udiv16:"); /* unsigned: __zpR2/__zpR -> quotient __zpR2, remainder __zpT0 */
    emit("    LDA #0");
    emit("    STA __zpT0");
    emit("    STA __zpT0+1");
    emit("    LDX #16");
    emit("__rt_udiv16_loop:");
    emit("    ASL __zpR2");
    emit("    ROL __zpR2+1");
    emit("    ROL __zpT0");
    emit("    ROL __zpT0+1");
    emit("    LDA __zpT0");
    emit("    SEC");
    emit("    SBC __zpR");
    emit("    TAY");
    emit("    LDA __zpT0+1");
    emit("    SBC __zpR+1");
    emit("    BCC __rt_udiv16_skip");
    emit("    STA __zpT0+1");
    emit("    STY __zpT0");
    emit("    INC __zpR2");
    emit("__rt_udiv16_skip:");
    emit("    DEX");
    emit("    BNE __rt_udiv16_loop");
    emit("    RTS");
    emit(" ");
    emit("__rt_neg_r2:");
    emit("    SEC");
    emit("    LDA #0");
    emit("    SBC __zpR2");
    emit("    TAX");
    emit("    LDA #0");
    emit("    SBC __zpR2+1");
    emit("    STA __zpR2+1");
    emit("    STX __zpR2");
    emit("    RTS");
    emit(" ");
    emit("__rt_neg_r:");
    emit("    SEC");
    emit("    LDA #0");
    emit("    SBC __zpR");
    emit("    TAX");
    emit("    LDA #0");
    emit("    SBC __zpR+1");
    emit("    STA __zpR+1");
    emit("    STX __zpR");
    emit("    RTS");
    emit(" ");
    emit("__rt_neg_t0:");
    emit("    SEC");
    emit("    LDA #0");
    emit("    SBC __zpT0");
    emit("    TAX");
    emit("    LDA #0");
    emit("    SBC __zpT0+1");
    emit("    STA __zpT0+1");
    emit("    STX __zpT0");
    emit("    RTS");
    emit(" ");
    /* Signed division/modulo, layered on top of the unsigned primitive
     * above using the standard technique: remember each operand's
     * sign, negate both to make them non-negative, do unsigned
     * division on the absolute values, then fix the signs of the
     * results afterward. C99's rules (which this matches) are that
     * division truncates toward zero and the remainder takes the
     * sign of the dividend - so the quotient's sign is the XOR of the
     * two input signs (negative iff exactly one operand was negative),
     * while the remainder just takes the dividend's original sign
     * unconditionally. __zpT1 holds the two 1-bit sign flags (as
     * whole bytes, one per operand) only for the duration of this
     * routine. */
    emit("__rt_sdivmod16:"); /* signed: __zpR2=dividend,__zpR=divisor -> __zpR2=quot, __zpT0=rem */
    emit("    LDA #0");
    emit("    STA __zpT1");
    emit("    STA __zpT1+1");
    emit("    LDA __zpR2+1");
    emit("    BPL __rt_sdm_d1");
    emit("    JSR __rt_neg_r2");
    emit("    INC __zpT1");
    emit("__rt_sdm_d1:");
    emit("    LDA __zpR+1");
    emit("    BPL __rt_sdm_d2");
    emit("    JSR __rt_neg_r");
    emit("    INC __zpT1+1");
    emit("__rt_sdm_d2:");
    emit("    JSR __rt_udiv16");
    emit("    LDA __zpT1");
    emit("    EOR __zpT1+1");
    emit("    AND #1");
    emit("    BEQ __rt_sdm_qdone");
    emit("    JSR __rt_neg_r2");
    emit("__rt_sdm_qdone:");
    emit("    LDA __zpT1");
    emit("    BEQ __rt_sdm_rdone");
    emit("    JSR __rt_neg_t0");
    emit("__rt_sdm_rdone:");
    emit("    RTS");
    emit(" ");
    /* == and != are the easy comparisons: two 16-bit values are equal
     * exactly when both bytes match, so a straight byte-by-byte CMP
     * is all that's needed - no sign handling, unlike < > <= >= below. */
    emit("__rt_eq16:");
    emit("    LDA __zpR2");
    emit("    CMP __zpR");
    emit("    BNE __rt_eq16_ne");
    emit("    LDA __zpR2+1");
    emit("    CMP __zpR+1");
    emit("    BNE __rt_eq16_ne");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_eq16_ne:");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_ne16:");
    emit("    LDA __zpR2");
    emit("    CMP __zpR");
    emit("    BNE __rt_ne16_ne");
    emit("    LDA __zpR2+1");
    emit("    CMP __zpR+1");
    emit("    BNE __rt_ne16_ne");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ne16_ne:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    /* Signed ordering comparisons (< > <= >=) are the one place a 6502
     * program has to work around a real hardware limitation: SBC only
     * sets the V (overflow) flag correctly for an 8-bit subtraction,
     * but these are 16-bit values, and the sign of a 16-bit
     * subtraction is only reliably indicated by the N flag after the
     * HIGH byte's SBC - and even then, only once corrected for
     * overflow. The standard fix: after subtracting the high bytes,
     * if V is set (the subtraction overflowed, so N has the WRONG
     * sign), flip the sign bit of the result with EOR #$80 before
     * testing it. These two routines do exactly that subtraction (one
     * for R2-R, one for R-R2, since which operand is "left" matters
     * for < vs >) and leave the corrected sign in the N flag for the
     * caller to test with BMI/BPL immediately after - they don't
     * store a 0/1 result themselves, unlike every other comparison
     * routine here. That's why the four routines below each start
     * with a JSR to one of these before turning the flag into 0 or 1. */
    emit("__rt_sub_r2r:"); /* computes R2-R (signed, overflow-corrected N flag), non-destructive */
    emit("    SEC");
    emit("    LDA __zpR2");
    emit("    SBC __zpR");
    emit("    LDA __zpR2+1");
    emit("    SBC __zpR+1");
    emit("    BVC __rt_sub_r2r_ok");
    emit("    EOR #$80");
    emit("__rt_sub_r2r_ok:");
    emit("    RTS");
    emit(" ");
    emit("__rt_sub_rr2:"); /* computes R-R2 (signed, overflow-corrected N flag) */
    emit("    SEC");
    emit("    LDA __zpR");
    emit("    SBC __zpR2");
    emit("    LDA __zpR+1");
    emit("    SBC __zpR2+1");
    emit("    BVC __rt_sub_rr2_ok");
    emit("    EOR #$80");
    emit("__rt_sub_rr2_ok:");
    emit("    RTS");
    emit(" ");
    /* < > <= >= all reduce to "is (R2-R) negative?" or "is (R-R2)
     * negative?" using the sign-corrected subtraction above, just
     * picking which subtraction and which of BMI/BPL answers each
     * question: R2<R is "R2-R is negative"; R2>=R is its exact
     * opposite; R2>R is "R-R2 is negative" (i.e. R<R2); R2<=R is ITS
     * opposite. Working it out by hand for each of the four cases is
     * worth doing once on paper if this isn't already familiar - it's
     * a classic technique, but not an obvious one on first encounter. */
    emit("__rt_lt16:"); /* R2 < R (signed) */
    emit("    JSR __rt_sub_r2r");
    emit("    BMI __rt_lt16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_lt16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_ge16:"); /* R2 >= R (signed) */
    emit("    JSR __rt_sub_r2r");
    emit("    BPL __rt_ge16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ge16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_gt16:"); /* R2 > R (signed) */
    emit("    JSR __rt_sub_rr2");
    emit("    BMI __rt_gt16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_gt16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_le16:"); /* R2 <= R (signed) */
    emit("    JSR __rt_sub_rr2");
    emit("    BPL __rt_le16_t");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_le16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    /* Shifts by a variable, runtime-known amount (__zpR2 << __zpR):
     * simply shift one bit at a time, __zpR times, using X as the
     * count-down counter. Nothing clever - the 6502 has no barrel
     * shifter, so there's no faster way to shift by an amount that
     * isn't known until runtime. */
    emit("__rt_shl16:"); /* __zpR2 << __zpR -> __zpR */
    emit("    LDX __zpR");
    emit("    LDA __zpR2");
    emit("    STA __zpR");
    emit("    LDA __zpR2+1");
    emit("    STA __zpR+1");
    emit("__rt_shl16_loop:");
    emit("    CPX #0");
    emit("    BEQ __rt_shl16_done");
    emit("    ASL __zpR");
    emit("    ROL __zpR+1");
    emit("    DEX");
    emit("    JMP __rt_shl16_loop");
    emit("__rt_shl16_done:");
    emit("    RTS");
    emit(" ");
    /* Right shift is arithmetic (sign-extending), matching how this
     * compiler treats `int` as signed: the classic 6502 idiom for
     * shifting a sign bit in from the left is `CMP #$80` on the high
     * byte before the ROR - CMP sets the carry flag to exactly the
     * value of the high bit being tested (carry set iff the byte is
     * >= $80), without modifying the byte itself, and ROR then
     * shifts that carry in as the new top bit. Repeated __zpR times. */
    emit("__rt_shr16:"); /* __zpR2 >> __zpR -> __zpR (arithmetic) */
    emit("    LDX __zpR");
    emit("    LDA __zpR2");
    emit("    STA __zpR");
    emit("    LDA __zpR2+1");
    emit("    STA __zpR+1");
    emit("__rt_shr16_loop:");
    emit("    CPX #0");
    emit("    BEQ __rt_shr16_done");
    emit("    LDA __zpR+1");
    emit("    CMP #$80");
    emit("    ROR __zpR+1");
    emit("    ROR __zpR");
    emit("    DEX");
    emit("    JMP __rt_shr16_loop");
    emit("__rt_shr16_done:");
    emit("    RTS");
    emit(" ");
    /* Runtime PETSCII case-mapping, used by both putchar() and puts()
     * (see codegen_expr.c's gen_call()) so that a character computed
     * or read from memory at runtime - not just a compile-time string
     * literal - still prints in the right case. Mirrors the exact
     * same a-z/A-Z remapping as c64asm.c's own ascii_to_petscii(), so
     * this compiler's output always matches what the existing
     * assembler's .text/.byte directives would produce for the same
     * text. See the "PETSCII and case" section of the project README
     * for why this mapping exists at all and what problem it solves. */
    emit("__rt_topetscii:"); /* mirrors c64asm.c's ascii_to_petscii, for runtime chars */
    emit("    LDA __zpR");
    emit("    CMP #$61");
    emit("    BCC __rt_tp_notlower");
    emit("    CMP #$7B");
    emit("    BCS __rt_tp_notlower");
    emit("    SEC");
    emit("    SBC #$20");
    emit("    STA __zpR");
    emit("    JMP __rt_tp_done");
    emit("__rt_tp_notlower:");
    emit("    CMP #$41");
    emit("    BCC __rt_tp_done");
    emit("    CMP #$5B");
    emit("    BCS __rt_tp_done");
    emit("    CLC");
    emit("    ADC #$80");
    emit("    STA __zpR");
    emit("__rt_tp_done:");
    emit("    RTS");
    emit(" ");
    /* Unsigned comparisons, structurally identical to the signed ones
     * above but simpler: memory addresses (pointers) are inherently
     * unsigned quantities, and 6502's plain CMP already compares
     * unsigned correctly with no overflow-flag correction needed, so
     * these just compare high bytes first, falling through to low
     * bytes only when the high bytes are equal - no sign-flag
     * gymnastics required. Used instead of the signed routines above
     * whenever codegen_expr.c knows at least one operand of a
     * comparison is a pointer (see its unsignedCmp handling). */
    emit("__rt_ult16:"); /* R2 < R (unsigned) - used for pointer comparisons */
    emit("    LDA __zpR2+1");
    emit("    CMP __zpR+1");
    emit("    BCC __rt_ult16_t");
    emit("    BNE __rt_ult16_f");
    emit("    LDA __zpR2");
    emit("    CMP __zpR");
    emit("    BCC __rt_ult16_t");
    emit("__rt_ult16_f:");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ult16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_ugt16:"); /* R2 > R (unsigned) */
    emit("    LDA __zpR+1");
    emit("    CMP __zpR2+1");
    emit("    BCC __rt_ugt16_t");
    emit("    BNE __rt_ugt16_f");
    emit("    LDA __zpR");
    emit("    CMP __zpR2");
    emit("    BCC __rt_ugt16_t");
    emit("__rt_ugt16_f:");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ugt16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_ule16:"); /* R2 <= R (unsigned) = NOT (R2 > R) */
    emit("    LDA __zpR+1");
    emit("    CMP __zpR2+1");
    emit("    BCC __rt_ule16_f");
    emit("    BNE __rt_ule16_t");
    emit("    LDA __zpR");
    emit("    CMP __zpR2");
    emit("    BCC __rt_ule16_f");
    emit("__rt_ule16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_ule16_f:");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    emit("__rt_uge16:"); /* R2 >= R (unsigned) = NOT (R2 < R) */
    emit("    LDA __zpR2+1");
    emit("    CMP __zpR+1");
    emit("    BCC __rt_uge16_f");
    emit("    BNE __rt_uge16_t");
    emit("    LDA __zpR2");
    emit("    CMP __zpR");
    emit("    BCC __rt_uge16_f");
    emit("__rt_uge16_t:");
    emit("    LDA #1");
    emit("    STA __zpR");
    emit("    LDA #0");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit("__rt_uge16_f:");
    emit("    LDA #0");
    emit("    STA __zpR");
    emit("    STA __zpR+1");
    emit("    RTS");
    emit(" ");
    /* Print a null-terminated string: walk __zpR as a pointer (copying
     * it into __zpAP, the one register that's allowed to be an
     * indirection target - see emit_zp_equates() below for why),
     * PETSCII-converting and printing one byte at a time via
     * __rt_topetscii + CHROUT until a 0 byte is found. This is also
     * exactly the piece of code that motivated moving __zpAP to a
     * genuinely safe zero-page address: this loop keeps its walking
     * pointer alive *across multiple JSR CHROUT calls*, which is
     * precisely the situation where a zero-page collision with the
     * KERNAL would corrupt it mid-string. See the zero-page comment
     * below for the full story. */
    emit("__rt_puts:"); /* walk a null-terminated string at __zpR, PETSCII-converting each byte */
    emit("    LDA __zpR");
    emit("    STA __zpAP");
    emit("    LDA __zpR+1");
    emit("    STA __zpAP+1");
    emit("__rt_puts_loop:");
    emit("    LDY #0");
    emit("    LDA (__zpAP),Y");
    emit("    BEQ __rt_puts_done");
    emit("    STA __zpR");
    emit("    JSR __rt_topetscii");
    emit("    LDA __zpR");
    emit("    JSR __CHROUT");
    emit("    CLC");
    emit("    LDA __zpAP");
    emit("    ADC #1");
    emit("    STA __zpAP");
    emit("    LDA __zpAP+1");
    emit("    ADC #0");
    emit("    STA __zpAP+1");
    emit("    JMP __rt_puts_loop");
    emit("__rt_puts_done:");
    emit("    RTS");
    emit(" ");
}

/* ===================================================================
 * String literals
 * ===================================================================
 * A string literal like "hello" needs to exist somewhere as actual
 * bytes in the compiled program's memory - it can't just be an
 * immediate value the way a number literal can, since `puts()` needs
 * an address to walk. intern_string() is called once per string
 * literal encountered anywhere in the program (see N_STR in
 * codegen_expr.c's gen_expr_to_R()), and just records its content and
 * invents it a label (__str_0, __str_1, ...); the actual bytes aren't
 * written out until emit_string_literals() runs, once, after all of
 * pass_b() has finished and every string literal in the program is
 * known (see main.c).
 *
 * The bytes are stored EXACTLY as written in the C source (after
 * escape processing - see lexer.c's read_escape()), not pre-converted
 * to PETSCII. That's deliberate: __rt_puts (above) converts each byte
 * to PETSCII at print time, the same way putchar() does, so storing
 * already-converted bytes here would double-convert them. Keeping
 * the stored data in its "natural" form also means a program that
 * builds or copies a string into its own buffer at runtime (see
 * tests/pointers.c's copy_str()) gets exactly the same printed output
 * as printing the original literal directly. */
typedef struct { char label[32]; char *content; } StrLit;
static StrLit g_strlits[1024];
static int g_nstrlits = 0;

char *intern_string(const char *s) {
    if (g_nstrlits >= (int)(sizeof(g_strlits)/sizeof(g_strlits[0])))
        fatal(0, "too many string literals");
    StrLit *sl = &g_strlits[g_nstrlits];
    snprintf(sl->label, sizeof(sl->label), "__str_%d", g_nstrlits);
    sl->content = xstrdup(s);
    g_nstrlits++;
    return sl->label;
}

/* Emits every interned string literal as raw `.byte` data, terminated
 * by an explicit 0 (so __rt_puts knows where each string ends). Using
 * numeric `.byte N,N,N,...` rather than c64asm.c's quoted-string form
 * (`.byte "..."`) is what avoids that directive's own built-in
 * ASCII->PETSCII conversion - these bytes need to stay exactly as
 * they are in the source, for the reasons explained above. */
void emit_string_literals(void) {
    if (g_nstrlits == 0) return;
    emit("; ---- string literals (raw bytes; __rt_puts converts at print time) ----");
    for (int i = 0; i < g_nstrlits; i++) {
        emit("%s:", g_strlits[i].label);
        fprintf(g_out, "    .byte ");
        for (const unsigned char *p = (const unsigned char *)g_strlits[i].content; *p; p++)
            fprintf(g_out, "%d,", (int)*p);
        fprintf(g_out, "0\n");
    }
    emit(" ");
}
