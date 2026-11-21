// The top's assertion reads a register inside the child; its landing in the
// child observes the register as u_dut.r.
module observe_child(input clk, input [2:0] a);
    reg [3:0] r = 0;
    always @(posedge clk) r <= {1'b0, a};
endmodule

module observe_top(input clk, input [2:0] b);
    observe_child u_child(.clk(clk), .a(b));
    p_internal: assert property (@(posedge clk) u_child.r < 4'd8);
endmodule
