// The parent saturates the child's input at 5; veval passes this on as a clamp.
module clamp_child(input clk, input [3:0] x);
    p_bound: assert property (@(posedge clk) x != 4'hf);
endmodule

module clamp_top(input clk, input [3:0] a);
    wire [3:0] c = a > 4'd5 ? 4'd5 : a;
    clamp_child u_child(.clk(clk), .x(c));
endmodule
