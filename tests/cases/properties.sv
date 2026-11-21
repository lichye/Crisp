module leaf(input clk, input [1:0] x);
    q_leaf: assert property (@(posedge clk) x == x);
endmodule

module top(input clk, input [1:0] a, input en);
    reg [1:0] c = 0;
    always @(posedge clk) if (en) c <= c + 1;

    p_one: assert property (@(posedge clk) a == a);
    p_two: assert property (@(posedge clk) a[0] == a[0]);
    // holds only because of the assumption below
    p_still: assert property (@(posedge clk) c == 0);
    en_low: assume property (@(posedge clk) !en);

    leaf u_leaf(.clk(clk), .x(a));

    for (genvar i = 0; i < 2; i++) begin : g
        p_gen: assert property (@(posedge clk) a[i] == a[i]);
    end
endmodule
