module order_shallow(input clk, input [2:0] a, output reg [3:0] y);
    initial y = 0;
    always @(posedge clk) y <= {1'b0, a};
endmodule

module order_top(input clk, input [2:0] a, b);
    wire [3:0] shallow, deep;
    order_shallow u_shallow(.clk(clk), .a(a), .y(shallow));
    up_mid u_mid(.clk(clk), .a(b), .y(deep));

    a_shallow: assert property (@(posedge clk) shallow < 4'd8);
    b_deep: assert property (@(posedge clk) deep < 4'd8);
    c_deep: assert property (@(posedge clk) deep != 4'd15);
endmodule
