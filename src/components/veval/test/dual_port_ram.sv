module dual_port_ram(
    input logic clk, ena, enb, a, b,
    input logic [1:0] da, db
);
    logic [1:0] mem[0:1];
    logic [1:0] read_word;

    always @(posedge clk) begin
        if (ena)
            mem[a] <= da;
        read_word <= mem[a];
    end

    always @(posedge clk) begin
        if (enb)
            mem[b] <= db;
    end

    logic valid = 0;
    logic pa, pb, pea, peb;
    logic [1:0] pda, pdb, old_a, old_b;
    always @(posedge clk) begin
        valid <= 1;
        {pa, pb, pea, peb, pda, pdb} <= {a, b, ena, enb, da, db};
        {old_a, old_b} <= {mem[0], mem[1]};
    end

    wire collision = pea && peb && pa == pb;
    wrote_a: assert property (@(posedge clk)
        valid && pea && !collision |-> mem[pa] == pda);
    wrote_b: assert property (@(posedge clk)
        valid && peb && !collision |-> mem[pb] == pdb);
    held_0: assert property (@(posedge clk)
        valid && !(pea && !pa) && !(peb && !pb) |-> mem[0] == old_a);
    held_1: assert property (@(posedge clk)
        valid && !(pea && pa) && !(peb && pb) |-> mem[1] == old_b);
    read_old: assert property (@(posedge clk)
        valid |-> read_word == (pa ? old_b : old_a));
    whole_word: assert property (@(posedge clk)
        valid && collision |-> mem[pa] == pda || mem[pa] == pdb);
    only_a: assert property (@(posedge clk)
        valid && collision |-> mem[pa] == pda);
    only_b: assert property (@(posedge clk)
        valid && collision |-> mem[pa] == pdb);
endmodule
