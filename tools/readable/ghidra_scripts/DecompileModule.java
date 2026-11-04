// Decompile one module of the game, on the function boundaries and jump tables tools/funcs.py found.
// Args: <work> <module> <out>. Writes <out>/<module>/<address>.c and <out>/<module>/stats.tsv:
//   address  status  lines  gotos  warnings  baddata  unrecovered_jumptables  vfpu_calls  seconds
// Run by tools/readable/run.sh. The program holds the module at its base; an overlay also gets the EBOOT's bytes, so
// its references to the EBOOT's data read as they do in the game.
//@category MHFU

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.pcode.JumpTable;
import ghidra.program.model.symbol.SourceType;

public class DecompileModule extends GhidraScript {
    private static final long RAM_END = 0x0A000000L;
    private static final Pattern GOTO = Pattern.compile("\\bgoto\\b");
    private static final Pattern VFPU = Pattern.compile("\\bv[a-z0-9]+_[sptq]\\s*\\(");

    private Address at(long a) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
    }

    private static int count(Pattern p, String s) {
        Matcher m = p.matcher(s);
        int n = 0;
        while (m.find()) n++;
        return n;
    }

    private static int count(String needle, String s) {
        int n = 0;
        for (int i = s.indexOf(needle); i >= 0; i = s.indexOf(needle, i + 1)) n++;
        return n;
    }

    /** The EBOOT's bytes in an overlay's program, and unknown memory around them so addresses stay valid. */
    private void mapRest(Path work, String module) throws Exception {
        long base = 0, size = 0, ebootBase = 0, ebootSize = 0;
        for (String line : Files.readAllLines(work.resolve("modules.tsv"))) {
            String[] f = line.split("\t");
            if (f[0].equals(module)) { base = Long.parseLong(f[1], 16); size = Long.parseLong(f[2], 16); }
            if (f[0].equals("EBOOT")) { ebootBase = Long.parseLong(f[1], 16); ebootSize = Long.parseLong(f[2], 16); }
        }
        Memory mem = currentProgram.getMemory();
        long end = ebootBase + ebootSize;
        if (!module.equals("EBOOT")) {
            try (InputStream in = Files.newInputStream(work.resolve("EBOOT/image.bin"))) {
                mem.createInitializedBlock("eboot", at(ebootBase), in, ebootSize, monitor, false);
            }
            if (base > end) mem.createUninitializedBlock("low", at(end), base - end, false);
            end = Math.max(end, base + size);
        }
        if (end < RAM_END) mem.createUninitializedBlock("high", at(end), RAM_END - end, false);
        // All of it writable: the decompiler reads read-only memory as constants, and the game's data is not constant
        for (MemoryBlock block : mem.getBlocks()) block.setWrite(true);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path work = Paths.get(args[0]);
        String module = args[1];
        Path out = Paths.get(args[2], module);
        Files.createDirectories(out);
        mapRest(work, module);

        List<String[]> funcs = new ArrayList<>();
        for (String line : Files.readAllLines(work.resolve(module).resolve("funcs.tsv"))) funcs.add(line.split("\t", -1));

        // Disassemble and create every function first, so calls between them resolve
        for (String[] f : funcs) {
            monitor.checkCancelled();
            long start = Long.parseLong(f[0], 16), end = Long.parseLong(f[1], 16);
            if (end <= start) continue;
            AddressSet body = new AddressSet(at(start), at(end - 1));
            new DisassembleCommand(body, body, true).applyTo(currentProgram, monitor);
            FunctionManager fm = currentProgram.getFunctionManager();
            List<Function> others = new ArrayList<>();
            fm.getFunctionsOverlapping(body).forEachRemaining(others::add);
            for (Function other : others) {
                println("overlap: " + f[0] + " with " + other.getName() + " " + other.getBody());
                fm.removeFunction(other.getEntryPoint());
            }
            fm.createFunction("f_" + module + "_" + f[0].toUpperCase(), at(start), body, SourceType.USER_DEFINED);
        }
        // Then the jump tables, known from funcs.py rather than left to the decompiler
        int tables = 0, tableErrors = 0;
        for (String[] f : funcs) {
            if (f[3].isEmpty()) continue;
            Function fn = getFunctionAt(at(Long.parseLong(f[0], 16)));
            if (fn == null) continue;
            for (String t : f[3].split(";")) {
                String[] p = t.split(":");
                ArrayList<Address> targets = new ArrayList<>();
                for (String d : p[1].split(",")) targets.add(at(Long.parseLong(d, 16)));
                try {
                    new JumpTable(at(Long.parseLong(p[0], 16)), targets, true, 0).writeOverride(fn);
                    tables++;
                } catch (Exception e) {
                    tableErrors++;
                    println("table " + p[0] + ": " + e);
                }
            }
        }
        println("functions " + funcs.size() + ", jump tables " + tables + " (" + tableErrors + " errors)");

        DecompInterface decomp = new DecompInterface();
        DecompileOptions options = new DecompileOptions();
        decomp.setOptions(options);
        decomp.toggleCCode(true);
        decomp.toggleSyntaxTree(true);
        decomp.setSimplificationStyle("decompile");
        if (!decomp.openProgram(currentProgram)) throw new IOException(decomp.getLastMessage());

        try (BufferedWriter stats = Files.newBufferedWriter(out.resolve("stats.tsv"), StandardCharsets.UTF_8)) {
            for (String[] f : funcs) {
                monitor.checkCancelled();
                String addr = f[0].toUpperCase();
                Function fn = getFunctionAt(at(Long.parseLong(f[0], 16)));
                long t0 = System.nanoTime();
                String status, code = "";
                if (fn == null) {
                    status = "nofunction";
                } else {
                    DecompileResults r = decomp.decompileFunction(fn, 60, monitor);
                    if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                        status = "ok";
                        code = r.getDecompiledFunction().getC();
                    } else {
                        status = "failed: " + String.valueOf(r.getErrorMessage()).replaceAll("\\s+", " ");
                    }
                }
                double secs = (System.nanoTime() - t0) / 1e9;
                Files.writeString(out.resolve(addr + ".c"),
                    "// " + module + " " + addr + " " + status + " (generated: f_" + module + "_" + addr + ")\n" + code);
                stats.write(String.join("\t", addr, status,
                    Integer.toString(count("\n", code)), Integer.toString(count(GOTO, code)),
                    Integer.toString(count("WARNING", code)), Integer.toString(count("halt_baddata", code)),
                    Integer.toString(count("UNRECOVERED_JUMPTABLE", code)), Integer.toString(count(VFPU, code)),
                    String.format("%.2f", secs)));
                stats.write("\n");
            }
        }
        decomp.dispose();
    }
}
