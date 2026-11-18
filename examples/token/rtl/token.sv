module token(input wire clk, input wire swap, input wire sample, output wire conflict);
    reg left = 1'b1;
    reg right = 1'b0;

    always @(posedge clk)
        if (swap) begin
            left  <= right;
            right <= left;
        end

    assign conflict = sample && left && right;
endmodule

module controller(input wire clk, input wire step, input wire enable,
                  input wire sample, output wire conflict);
    token state(.clk(clk), .swap(step && enable), .sample(sample), .conflict(conflict));
endmodule
