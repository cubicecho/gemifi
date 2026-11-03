// SPDX-License-Identifier: MIT
package gemifi.bitwig;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.bitwig.extension.ExtensionDefinition;
import com.bitwig.extension.controller.ControllerExtensionDefinition;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.ServiceLoader;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

/** The parts of the Bitwig side that can be checked without Bitwig. */
class PackagingTest {
    @Test
    void definitionIsFoundAsAService() {
        List<ExtensionDefinition> found = new ArrayList<>();
        ServiceLoader.load(ExtensionDefinition.class).forEach(found::add);
        assertEquals(1, found.size());
        ControllerExtensionDefinition d = (ControllerExtensionDefinition) found.get(0);
        assertEquals("gemifi", d.getName());
        assertEquals("gemifi", d.getHardwareVendor());
        assertEquals("gemifi", d.getHardwareModel());
        assertEquals(1, d.getNumMidiInPorts());
        assertEquals(1, d.getNumMidiOutPorts());
        assertTrue(d.getRequiredAPIVersion() <= 25);
    }

    @Test
    void coreHasNoBitwigImports() throws IOException {
        Path core = Paths.get("src/main/java/gemifi/bitwig/core");
        try (Stream<Path> files = Files.list(core)) {
            for (Path p : (Iterable<Path>) files::iterator) {
                String src = new String(Files.readAllBytes(p), StandardCharsets.UTF_8);
                assertFalse(src.contains("com.bitwig"), p + " imports Bitwig");
                assertTrue(src.startsWith("// SPDX-License-Identifier: MIT"), p + " lacks the SPDX line");
            }
        }
    }
}
