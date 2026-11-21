// The child alone can see an odd x; the parent's register only ever holds even values.
module even_child(input clk, input [3:0] x);
    p_even: assert property (@(posedge clk) x[0] == 1'b0);
endmodule

module even_top(input clk, input [2:0] a);
    reg [3:0] r = 0;
    always @(posedge clk) r <= {a, 1'b0};
    even_child u_child(.clk(clk), .x(r));
endmodule
