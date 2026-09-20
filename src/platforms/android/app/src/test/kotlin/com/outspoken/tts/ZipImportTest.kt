package com.outspoken.tts

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class ZipImportTest {
    @get:Rule val temp = TemporaryFolder()

    /** The shape the desktop extractor makes, under `prefix`: two engines
     * and two voice folders, with the marker files the catalogue wants. */
    private fun data(prefix: String): Map<String, ByteArray> = linkedMapOf(
        prefix + "macintalk2/Cecy_1.bin" to "cecy one".toByteArray(),
        prefix + "macintalk2/Cecy_3.bin" to "cecy three".toByteArray(),
        prefix + "macintalk2/ttss_0.bin" to byteArrayOf(1, 2, 3),
        prefix + "macintalkpro/gtse_1.bin" to "pro".toByteArray(),
        prefix + "macintalkpro/datafork.bin" to "lexicon".toByteArray(),
        prefix + "macintalkpro/rsrcfork.bin" to "resources".toByteArray(),
        prefix + "voices/Ben/ttvd_1.bin" to byteArrayOf(0, 1, 2, 3, 127),
        prefix + "voices/Bruce/ttvd_110.bin" to byteArrayOf(9, 9),
        prefix + "voices/Bruce/rsrcfork.bin" to byteArrayOf(4),
    )

    private fun archive(entries: Map<String, ByteArray>): File = temp.newFile().also { file ->
        // No explicit directory entries: common ZIP writers omit them.
        ZipOutputStream(file.outputStream()).use { zip ->
            for ((name, bytes) in entries) {
                zip.putNextEntry(ZipEntry(name)); zip.write(bytes); zip.closeEntry()
            }
        }
    }

    private fun sources(file: File) = listOf(ZipImport.source(file),
        ZipImport.Source(file.name, file.length(), { file.inputStream() }))

    @Test fun everyWrapperImportsFlatFromFilesAndStreams() {
        for (prefix in listOf("", "outspoken/", "OUTSPOKEN/", "outspoken-data/",
                "outspoken-data/outspoken/", "macintalk/outspoken/", "My Backup/outspoken/")) {
            val entries = data(prefix)
            val file = archive(entries + ("unrelated.txt" to byteArrayOf(9)))
            for (source in sources(file)) {
                val plan = ZipImport.inspect(source)
                assertNull("$prefix: ${plan.refusal}", plan.refusal)
                assertEquals("$prefix", listOf("macintalk2", "macintalkpro", "voices"), plan.units)
                assertEquals(2, plan.found.first { it.unit == "voices" }.voices)
                val root = temp.newFolder()
                assertEquals(listOf("macintalk2", "macintalkpro", "voices"),
                    ZipImport.extract(source, plan, root, { _, _ -> }, { false }))
                for ((name, bytes) in entries)
                    assertArrayEquals(name, bytes, File(root, name.removePrefix(prefix)).readBytes())
                assertEquals(setOf("macintalk2", "macintalkpro", "voices"), root.list()!!.toSet())
                assertFalse(File(root, "unrelated.txt").exists())
            }
        }
    }

    @Test fun anEngineFolderIsKnownByItsMarkerWhateverItIsCalled() {
        val file = archive(linkedMapOf(
            "stuff/MT3/ttvi_10.bin" to byteArrayOf(1), "stuff/MT3/ttvi_8.bin" to byteArrayOf(2),
            "stuff/MT3/ttvi_9.bin" to byteArrayOf(3), "stuff/MT3/ttss_0.bin" to byteArrayOf(4)))
        val plan = ZipImport.inspect(ZipImport.source(file))
        assertNull(plan.refusal)
        assertEquals(listOf("macintalk3"), plan.units)
        val root = temp.newFolder()
        ZipImport.extract(ZipImport.source(file), plan, root, { _, _ -> }, { false })
        assertTrue(File(root, "macintalk3/ttvi_10.bin").isFile)
        assertFalse(File(root, "stuff").exists())
    }

    @Test fun voicesMergeAndAnEngineIsReplaced() {
        val root = temp.newFolder()
        File(root, "voices/Fred").mkdirs(); File(root, "voices/Fred/ttvd_1.bin").writeBytes(byteArrayOf(7))
        File(root, "voices/Bruce").mkdirs(); File(root, "voices/Bruce/old.bin").writeBytes(byteArrayOf(8))
        File(root, "macintalk2").mkdirs(); File(root, "macintalk2/stale.bin").writeBytes(byteArrayOf(6))
        val file = archive(data("outspoken/"))
        val plan = ZipImport.inspect(ZipImport.source(file))
        ZipImport.extract(ZipImport.source(file), plan, root, { _, _ -> }, { false })
        assertTrue("a voice not in the zip stays", File(root, "voices/Fred/ttvd_1.bin").isFile)
        assertFalse("a voice in the zip is replaced whole", File(root, "voices/Bruce/old.bin").exists())
        assertTrue(File(root, "voices/Bruce/ttvd_110.bin").isFile)
        assertFalse("an engine folder is replaced whole", File(root, "macintalk2/stale.bin").exists())
        assertTrue(File(root, "macintalk2/Cecy_1.bin").isFile)
        assertFalse(File(root, "voices" + ZipImport.IMPORTING).exists())
    }

    @Test fun aPantheraZipIsSaidToBeOne() {
        val file = archive(linkedMapOf(
            "lion/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/Contents/MacOS/MacinTalk" to byteArrayOf(1)))
        val plan = ZipImport.inspect(ZipImport.source(file))
        assertNotNull(plan.refusal)
        assertTrue(plan.refusal!!, plan.refusal!!.contains("Panthera"))
    }

    @Test fun nothingRecognisableIsRefusedWithHelp() {
        val file = archive(linkedMapOf("photos/cat.jpg" to byteArrayOf(1), "notes.txt" to byteArrayOf(2)))
        val plan = ZipImport.inspect(ZipImport.source(file))
        assertNotNull(plan.refusal)
        assertTrue(plan.refusal!!, plan.refusal!!.contains("macintalk1"))
    }

    @Test fun theSameFolderTwiceIsRefused() {
        val file = archive(data("a/") + data("b/"))
        val plan = ZipImport.inspect(ZipImport.source(file))
        assertNotNull(plan.refusal)
        assertTrue(plan.refusal!!, plan.refusal!!.contains("twice"))
    }

    @Test fun aPathThatEscapesRefusesTheWholeZip() {
        val file = archive(data("") + ("macintalk2/../../escape.bin" to byteArrayOf(1)))
        for (source in sources(file)) {
            val plan = ZipImport.inspect(source)
            assertNotNull(plan.refusal)
            assertTrue(plan.refusal!!, plan.refusal!!.contains("leaves its own folder"))
        }
    }

    @Test fun finderNoiseAndBackslashesAreTolerated() {
        val file = archive(linkedMapOf(
            "outspoken\\macintalk2\\Cecy_1.bin" to byteArrayOf(1),
            "outspoken\\macintalk2\\Cecy_3.bin" to byteArrayOf(2),
            "__MACOSX/outspoken/macintalk2/._Cecy_1.bin" to byteArrayOf(3),
            "outspoken/.DS_Store" to byteArrayOf(4)))
        val plan = ZipImport.inspect(ZipImport.source(file))
        assertNull(plan.refusal)
        assertEquals(listOf("macintalk2"), plan.units)
        assertEquals(2, plan.found[0].files)
    }

    @Test fun aCancelledImportLeavesNothingBehind() {
        val root = temp.newFolder()
        val file = archive(data(""))
        val plan = ZipImport.inspect(ZipImport.source(file))
        var polls = 0
        try {
            ZipImport.extract(ZipImport.source(file), plan, root, { _, _ -> }) { ++polls > 2 }
            fail("expected the cancel")
        } catch (e: ZipImport.Cancelled) { /* expected */ }
        assertEquals(emptyList<String>(), root.list()!!.toList())
    }
}
