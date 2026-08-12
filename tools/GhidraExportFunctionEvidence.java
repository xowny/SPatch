// Export paired instruction-level and decompiler evidence for selected RVAs.
// @category SPatch

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public final class GhidraExportFunctionEvidence extends GhidraScript {
    @Override
    public void run() throws Exception {
        final String[] arguments = getScriptArgs();
        if (arguments.length < 2) {
            throw new IllegalArgumentException(
                "usage: <output-path> <rva> [<rva> ...]");
        }

        final File output = new File(arguments[0]);
        final File parent = output.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IllegalStateException(
                "could not create output directory: " + parent);
        }

        final DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException(
                "decompiler rejected program: " + decompiler.getLastMessage());
        }

        try (PrintWriter writer = new PrintWriter(new BufferedWriter(
                 new OutputStreamWriter(
                     new FileOutputStream(output), StandardCharsets.UTF_8)))) {
            writer.println("PROGRAM=" + currentProgram.getExecutablePath());
            writer.println("IMAGE_BASE=" + currentProgram.getImageBase());
            writer.println("LANGUAGE=" + currentProgram.getLanguageID());

            for (int index = 1; index < arguments.length; ++index) {
                monitor.checkCancelled();
                final long rva = Long.decode(arguments[index]);
                final Address target = currentProgram.getImageBase().add(rva);
                Function function =
                    currentProgram.getFunctionManager().getFunctionAt(target);
                if (function == null) {
                    function = currentProgram.getFunctionManager()
                        .getFunctionContaining(target);
                }
                boolean targetedDiscovery = false;
                if (currentProgram.getListing().getInstructionAt(target) == null) {
                    if (function != null &&
                        function.getEntryPoint().equals(target) &&
                        function.getBody().getNumAddresses() <= 1) {
                        currentProgram.getFunctionManager().removeFunction(target);
                        function = null;
                    }
                    if (disassemble(target)) {
                        if (function == null) {
                            function = createFunction(target, null);
                        }
                        targetedDiscovery = function != null;
                    }
                } else if (function == null) {
                    function = createFunction(target, null);
                    targetedDiscovery = function != null;
                }

                writer.println();
                writer.printf("=== RVA 0x%X ADDRESS %s ===%n", rva, target);
                writer.println("TARGETED_DISCOVERY=" + targetedDiscovery);
                if (function == null) {
                    writer.println("FUNCTION=<not-defined>");
                    continue;
                }

                writer.println("FUNCTION=" + function.getName());
                writer.println("ENTRY=" + function.getEntryPoint());
                writer.println("BODY=" + function.getBody());
                writer.println("--- DISASSEMBLY ---");
                final InstructionIterator instructions =
                    currentProgram.getListing().getInstructions(
                        function.getBody(), true);
                while (instructions.hasNext()) {
                    monitor.checkCancelled();
                    final Instruction instruction = instructions.next();
                    final byte[] bytes = instruction.getBytes();
                    final StringBuilder encoded = new StringBuilder();
                    for (final byte value : bytes) {
                        if (encoded.length() != 0) {
                            encoded.append(' ');
                        }
                        encoded.append(String.format("%02X", value & 0xff));
                    }
                    writer.printf("%s  %-47s  %s%n",
                                  instruction.getAddress(), encoded,
                                  instruction.toString());
                }

                writer.println("--- DECOMPILATION ---");
                final DecompileResults results =
                    decompiler.decompileFunction(function, 120, monitor);
                if (!results.decompileCompleted() ||
                    results.getDecompiledFunction() == null) {
                    writer.println("<decompilation-failed> " +
                                   results.getErrorMessage());
                } else {
                    writer.println(results.getDecompiledFunction().getC());
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
