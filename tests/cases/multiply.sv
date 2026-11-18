module multiply(input wire clk, input wire [31:0] a, b);
    wire [31:0] ab = a * b;
    wire [31:0] ba = b * a;
    p_commutes: assert property (@(posedge clk) ab == ba);
endmodule
