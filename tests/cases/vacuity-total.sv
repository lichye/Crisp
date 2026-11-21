module total(input bit clk, start);
    bit [15:0] age = 0;
    bit stuck = 0;
    always_ff @(posedge clk) begin
        if (age < 1200) age <= age + 1'b1;
        stuck <= stuck;
    end
`ifdef BLOCK_START
    block_start: assume property (@(posedge clk) !start);
`endif
    early: assert property (@(posedge clk) (age == 0 && start) |-> 1'b1);
    never: assert property (@(posedge clk) stuck |-> 1'b1);
endmodule
