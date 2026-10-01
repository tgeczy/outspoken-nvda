package com.outspoken.tts

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** Which folders a removal has to delete.
 *
 * The dangerous half of removing engine data is not the delete, it is the
 * arithmetic before it: a unit can sit in three base roots and in any wrapper
 * folder somebody zipped around it, `migrate` copies anything outside protected
 * storage back in on the next unlock, and a removal that missed a copy would
 * have the unit reappear by itself. That is pure file work, so it is tested
 * here rather than on a phone.
 */
class UnitRemovalTest {
    @get:Rule val temp = TemporaryFolder()

    private fun unit(root: File, name: String): File =
        File(root, name).also {
            it.mkdirs()
            File(it, "marker").writeText(name)
        }

    /** Every copy, in every root -- not only the one the host reads first. */
    @Test fun findsEveryCopyAcrossEveryRoot() {
        val protected = temp.newFolder("protected")
        val inbox = temp.newFolder("inbox")
        val wrapper = temp.newFolder("inbox", "outspoken")
        unit(protected, "macintalk3")
        unit(inbox, "macintalk3")
        unit(wrapper, "macintalk3")
        unit(protected, "macintalk2")          // a different unit stays out of it

        val found = OutspokenEngine.unitFolders(listOf(protected, inbox, wrapper), "macintalk3")
        assertEquals(setOf(File(protected, "macintalk3"), File(inbox, "macintalk3"),
                           File(wrapper, "macintalk3")), found.toSet())
    }

    /** A cancelled import or an interrupted move leaves a staging folder beside
     * the real one. Both belong to the unit and both go with it. */
    @Test fun includesStagingFolders() {
        val root = temp.newFolder("protected")
        unit(root, "macintalkpro")
        unit(root, "macintalkpro" + ProtectedStorage.MOVING)
        unit(root, "macintalkpro" + ZipImport.IMPORTING)

        val found = OutspokenEngine.unitFolders(listOf(root), "macintalkpro")
        assertEquals(3, found.size)
        assertTrue(found.contains(File(root, "macintalkpro" + ProtectedStorage.MOVING)))
        assertTrue(found.contains(File(root, "macintalkpro" + ZipImport.IMPORTING)))
    }

    /** The two Pro engines are separate folders, and the English one must never
     * be caught by removing the Spanish one -- or by a prefix match on either. */
    @Test fun theTwoProEnginesAreSeparate() {
        val root = temp.newFolder("protected")
        unit(root, "macintalkpro")
        unit(root, "macintalkespanol")

        assertEquals(listOf(File(root, "macintalkpro")),
                     OutspokenEngine.unitFolders(listOf(root), "macintalkpro"))
        assertEquals(listOf(File(root, "macintalkespanol")),
                     OutspokenEngine.unitFolders(listOf(root), "macintalkespanol"))
    }

    /** `voices` is one folder every engine reads, so it is a unit of its own
     * and removing an engine never takes it. */
    @Test fun removingAnEngineLeavesTheVoices() {
        val root = temp.newFolder("protected")
        unit(root, "macintalk2")
        val voices = unit(root, ZipImport.VOICES)

        for (f in OutspokenEngine.unitFolders(listOf(root), "macintalk2")) f.deleteRecursively()
        assertFalse(File(root, "macintalk2").exists())
        assertTrue("the voices every engine reads must survive", voices.isDirectory)
    }

    /** And the reverse: removing the voices leaves the engines standing, which
     * is why the dialog says what that costs. */
    @Test fun removingTheVoicesLeavesTheEngines() {
        val root = temp.newFolder("protected")
        val engine = unit(root, "macintalk3")
        unit(root, ZipImport.VOICES)

        for (f in OutspokenEngine.unitFolders(listOf(root), ZipImport.VOICES)) f.deleteRecursively()
        assertFalse(File(root, ZipImport.VOICES).exists())
        assertTrue(engine.isDirectory)
    }

    /** The same root listed twice -- which `candidateRoots` can do, since a
     * wrapper may resolve to a path already listed -- must not delete twice. */
    @Test fun oneEntryPerFolderHoweverManyTimesARootIsListed() {
        val root = temp.newFolder("protected")
        unit(root, "macintalk1")
        val found = OutspokenEngine.unitFolders(listOf(root, root, File(root.path)), "macintalk1")
        assertEquals(1, found.size)
    }

    /** Nothing installed is not an error, and neither is a unit nobody has. */
    @Test fun answersEmptyWhenThereIsNothingToRemove() {
        val root = temp.newFolder("protected")
        assertTrue(OutspokenEngine.unitFolders(listOf(root), "macintalk1").isEmpty())
        unit(root, "macintalk1")
        assertTrue(OutspokenEngine.unitFolders(listOf(root), "macintalk3").isEmpty())
    }

    /** A file where a unit folder would be is not a unit folder. Deleting it
     * would be deleting something this feature does not understand. */
    @Test fun ignoresAFileWithAUnitName() {
        val root = temp.newFolder("protected")
        File(root, "macintalk2").writeText("not a folder")
        assertTrue(OutspokenEngine.unitFolders(listOf(root), "macintalk2").isEmpty())
    }

    /** Every unit the importer knows is a unit this can remove: a name the
     * importer can write and the remover cannot find would be unreachable. */
    @Test fun everyImportableUnitIsRemovable() {
        val root = temp.newFolder("protected")
        for (name in ZipImport.UNITS) unit(root, name)
        for (name in ZipImport.UNITS)
            assertEquals("$name should be found", listOf(File(root, name)),
                         OutspokenEngine.unitFolders(listOf(root), name))
    }

    /** An empty folder wearing a unit's name is not an installed unit -- it is
     * what an interrupted import leaves -- but naming that unit still finds the
     * folder, so the leftover can be cleaned up rather than stranded. */
    @Test fun anEmptyUnitFolderIsStillFoundButIsNotInstalled() {
        val root = temp.newFolder("protected")
        File(root, "macintalk2").mkdirs()
        val found = OutspokenEngine.unitFolders(listOf(root), "macintalk2")
        assertEquals(listOf(File(root, "macintalk2")), found)
        assertFalse("an empty folder holds no files",
                    found.any { f -> f.walkTopDown().any { it.isFile } })
    }

    /** Each engine unit belongs to exactly one family, and `voices` to none --
     * which is what decides whose settings a removal forgets. */
    @Test fun eachEngineUnitNamesItsFamily() {
        assertEquals(OutspokenEngine.FAM_SP, OutspokenEngine.unitFamily("macintalk1"))
        assertEquals(OutspokenEngine.FAM_MTK2, OutspokenEngine.unitFamily("macintalk2"))
        assertEquals(OutspokenEngine.FAM_MTK3, OutspokenEngine.unitFamily("macintalk3"))
        assertEquals(OutspokenEngine.FAM_GALA, OutspokenEngine.unitFamily("macintalkpro"))
        assertEquals(OutspokenEngine.FAM_CAMI, OutspokenEngine.unitFamily("macintalkespanol"))
        assertEquals(null, OutspokenEngine.unitFamily(ZipImport.VOICES))
        // Every family is reachable, so no family's settings can be orphaned.
        assertEquals(OutspokenEngine.FAMILIES.toSet(),
                     ZipImport.UNITS.mapNotNull { OutspokenEngine.unitFamily(it) }.toSet())
    }
}
