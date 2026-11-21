// p_small lands in u_mid and in u_mid.u_leaf. The leaf alone may output 15;
// u_mid's register keeps the leaf's top input bit 0, so it holds one level up.
module up_leaf(input clk, input [3:0] x, output [3:0] y);
    assign y = x;
endmodule

module up_mid(input clk, input [2:0] a, output [3:0] y);
    reg [3:0] r = 0;
    always @(posedge clk) r <= {1'b0, a};
    up_leaf u_leaf(.clk(clk), .x(r), .y(y));
endmodule

module up_top(input clk, input [2:0] b);
    wire [3:0] y;
    up_mid u_mid(.clk(clk), .a(b), .y(y));
    p_small: assert property (@(posedge clk) y < 4'd8);
endmodule
