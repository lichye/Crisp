module reset_counter(input wire clk, rst_n, en);
    reg [3:0] c;
    always @(posedge clk)
        if (!rst_n) c <= 0;
        else if (en && c < 9) c <= c + 1'b1;
    p_range: assert property (@(posedge clk) !rst_n || c <= 9);
endmodule
