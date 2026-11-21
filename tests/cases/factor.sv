module factor(input wire clk, input wire [31:0] a, b);
    // 64'h78a54164268d1ca7 is the product of the 32-bit primes 2583493733 and 3364988443.
    // Refuting this takes SMT solvers far longer than a few seconds.
    p_no_factors: assert property (@(posedge clk) {32'b0, a} * {32'b0, b} != 64'h78a54164268d1ca7);
endmodule
