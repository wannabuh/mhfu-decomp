// Decompile one module of the game, on the function boundaries and jump tables tools/funcs.py found.
// Args: <work> <module> <out>. Writes <out>/<module>/<address>.c and <out>/<module>/stats.tsv:
//   address  status  lines  gotos  warnings  baddata  unrecovered_jumptables  vfpu_calls  seconds
// and <out>/<module>/signatures.tsv: address, return kind (v/i/f), parameter kinds. With <work>/signatures.tsv (those of
// a first pass, merged by tools/readable/all.sh) every function's signature is set from it, so a call shows the arguments
// the function reads even when their registers were set long before; functions of other modules become named stubs.
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
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.pcode.FunctionPrototype;
import ghidra.program.model.pcode.HighFunction;
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

    private static DataType kindType(char k) {
        return k == 'f' ? FloatDataType.dataType : k == 'v' ? VoidDataType.dataType : Undefined4DataType.dataType;
    }

    private static char kindOf(DataType t) {
        if (t instanceof VoidDataType) return 'v';
        return t instanceof FloatDataType ? 'f' : 'i';
    }

    /** Which module(s) each function start is in, over all modules: the same address can be in several overlays. */
    private Map<Long, List<String>> loadOwners(Path work) throws IOException {
        Map<Long, List<String>> owners = new HashMap<>();
        for (String line : Files.readAllLines(work.resolve("allfuncs.tsv"))) {
            String[] f = line.split("\t");
            owners.computeIfAbsent(Long.parseLong(f[0], 16), k -> new ArrayList<>()).add(f[1]);
        }
        return owners;
    }

    private String nameFor(String module, long addr, Map<Long, List<String>> owners) {
        List<String> o = owners.get(addr);
        String hex = Long.toHexString(addr).toUpperCase();
        if (o == null) return "f_unknown_" + hex;
        if (o.contains(module)) return "f_" + module + "_" + hex;
        return "f_" + (o.size() == 1 ? o.get(0) : "overlay") + "_" + hex;
    }

    /** Calls and tail jumps out of a function to where no function is yet: a stub there, named, so they read as calls. */
    private void linkTargets(String module, Map<Long, List<String>> owners) throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        List<Function> ours = new ArrayList<>();
        fm.getFunctions(true).forEachRemaining(ours::add);
        for (Function fn : ours) {
            InstructionIterator it = currentProgram.getListing().getInstructions(fn.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                for (Address t : ins.getFlows()) {
                    if (fn.getBody().contains(t)) continue;
                    // A jump out of the function is a tail call: said so, or the decompiler stops at it
                    if (ins.getFlowType().isJump() && ins.getFlowType().isUnConditional())
                        ins.setFlowOverride(FlowOverride.CALL_RETURN);
                    Function there = fm.getFunctionAt(t);
                    String name = nameFor(module, t.getOffset(), owners);
                    if (there == null && fm.getFunctionContaining(t) == null) {
                        fm.createFunction(name, t, new AddressSet(t, t), SourceType.USER_DEFINED);
                    } else if (there != null && there.getName().startsWith("FUN_")) {
                        there.setName(name, SourceType.USER_DEFINED);
                    }
                }
            }
        }
    }

    /** Signatures from a first pass: this module's own first, else what the owners of the address agree on. */
    private void applySignatures(Path work, String module, Map<Long, List<String>> owners) throws Exception {
        Path path = work.resolve("signatures.tsv");
        if (!Files.exists(path)) return;
        Map<String, String> sigs = new HashMap<>();
        for (String line : Files.readAllLines(path)) {
            String[] f = line.split("\t", 3);
            sigs.put(f[0] + " " + f[1], f[2]);
        }
        String cc = currentProgram.getCompilerSpec().getDefaultCallingConvention().getName();
        int applied = 0, disagreed = 0;
        FunctionIterator all = currentProgram.getFunctionManager().getFunctions(true);
        while (all.hasNext()) {
            Function fn = all.next();
            long addr = fn.getEntryPoint().getOffset();
            String hex = Long.toHexString(addr);
            String sig = sigs.get(module + " " + hex);
            if (sig == null) {
                Set<String> found = new HashSet<>();
                for (String o : owners.getOrDefault(addr, List.of())) {
                    String s = sigs.get(o + " " + hex);
                    if (s != null) found.add(s);
                }
                if (found.size() == 1) sig = found.iterator().next();
                else if (found.size() > 1) disagreed++;
            }
            if (sig == null) continue;
            String[] p = sig.split("\t", -1);
            List<Variable> params = new ArrayList<>();
            for (int i = 0; i < p[1].length(); i++)
                params.add(new ParameterImpl("param_" + (i + 1), kindType(p[1].charAt(i)), currentProgram));
            fn.updateFunction(cc, new ReturnParameterImpl(kindType(p[0].charAt(0)), currentProgram), params,
                Function.FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS, true, SourceType.ANALYSIS);
            applied++;
        }
        println("signatures applied " + applied + ", owners disagreed on " + disagreed);
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
        Map<Long, List<String>> owners = loadOwners(work);
        linkTargets(module, owners);
        applySignatures(work, module, owners);
        // Jump tables: the decompiler's own recovery, which gets the case labels right; funcs.py's table only as an
        // override where it fails (an override collapses repeated targets, and with them the labels)
        Map<String, String> tablesOf = new HashMap<>();
        for (String[] f : funcs) if (!f[3].isEmpty()) tablesOf.put(f[0], f[3]);
        println("functions " + funcs.size() + ", with jump tables " + tablesOf.size());

        DecompInterface decomp = new DecompInterface();
        DecompileOptions options = new DecompileOptions();
        decomp.setOptions(options);
        decomp.toggleCCode(true);
        decomp.toggleSyntaxTree(true);
        decomp.setSimplificationStyle("decompile");
        if (!decomp.openProgram(currentProgram)) throw new IOException(decomp.getLastMessage());

        try (BufferedWriter stats = Files.newBufferedWriter(out.resolve("stats.tsv"), StandardCharsets.UTF_8);
             BufferedWriter signatures = Files.newBufferedWriter(out.resolve("signatures.tsv"), StandardCharsets.UTF_8)) {
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
                    if (r.decompileCompleted() && tablesOf.containsKey(f[0])
                            && r.getDecompiledFunction().getC().contains("Could not recover jumptable")) {
                        boolean overridden = false;
                        for (String t : tablesOf.get(f[0]).split(";")) {
                            String[] p = t.split(":");
                            ArrayList<Address> targets = new ArrayList<>();
                            for (String d : p[1].split(",")) targets.add(at(Long.parseLong(d, 16)));
                            new JumpTable(at(Long.parseLong(p[0], 16)), targets, true, 0).writeOverride(fn);
                            overridden = true;
                        }
                        if (overridden) r = decomp.decompileFunction(fn, 60, monitor);
                    }
                    if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                        status = "ok";
                        code = r.getDecompiledFunction().getC();
                        HighFunction hf = r.getHighFunction();
                        if (hf != null) {
                            FunctionPrototype proto = hf.getFunctionPrototype();
                            StringBuilder kinds = new StringBuilder();
                            for (int i = 0; i < proto.getNumParams(); i++)
                                kinds.append(kindOf(proto.getParam(i).getDataType()) == 'f' ? 'f' : 'i');
                            signatures.write(f[0] + "\t" + kindOf(proto.getReturnType()) + "\t" + kinds + "\n");
                        }
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
