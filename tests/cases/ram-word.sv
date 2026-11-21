module ram_word(input clk);
    // A memory with a dynamic address: level word keeps it as an SMT array.
    reg [2:0] c = 0;
    reg [7:0] mem [0:7];
    initial for (int i = 0; i < 8; i++) mem[i] = 0;
    always @(posedge clk) begin
        c <= c + 1'b1;
        mem[c] <= {5'b0, c} + 8'd1;
    end
    // holds: the element written in the previous cycle has the value written
    p_written: assert property (@(posedge clk) c != 0 -> mem[c - 3'd1] == {5'b0, c});
    // fails at cycle 6, when element 5 has been written
    p_untouched: assert property (@(posedge clk) mem[5] == 0);
endmodule
