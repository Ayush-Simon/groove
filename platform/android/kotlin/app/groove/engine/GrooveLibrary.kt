package app.groove.engine

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.DocumentsContract
import org.json.JSONArray
import java.io.File
import java.lang.ref.WeakReference
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The SQLite-backed library index. Scans run on a native worker thread with their own database
 * connection; queries run on this class's background executor. Nothing here touches the main thread
 * except result delivery, so a 20 000-song scan can't cause an ANR.
 */
class GrooveLibrary private constructor(private val handle: Long) : AutoCloseable {

    /** Mirrors groove::Field — same order. */
    enum class Field { TITLE, ARTIST, ALBUM, ALBUM_ARTIST, GENRE, COMPOSER, CONTAINER, CODEC, FOLDER, PATH,
        YEAR, TRACK_NO, DISC_NO, SAMPLE_RATE, BIT_DEPTH, CHANNELS, DURATION, BITRATE, ADDED_AT, LOSSLESS }

    /** Mirrors groove::Op — same order. */
    enum class Op { EQUALS, NOT_EQUALS, CONTAINS, NOT_CONTAINS, STARTS_WITH, GREATER, GREATER_OR_EQUAL, LESS, LESS_OR_EQUAL, BETWEEN, IS_TRUE, IS_FALSE }

    /** Text rules use [text]; numeric rules use [a] (and [b] for BETWEEN); LOSSLESS uses IS_TRUE / IS_FALSE. */
    data class Rule(val field: Field, val op: Op, val text: String? = null, val a: Double = 0.0, val b: Double = 0.0)

    data class SmartPlaylist(
        val rules: List<Rule> = emptyList(),
        val matchAll: Boolean = true,
        val sortBy: Field = Field.ARTIST,
        val descending: Boolean = false,
        val limit: Int = 0,
        val random: Boolean = false,
    )

    data class Entry(val uri: String, val folder: String, val size: Long, val mtime: Long)

    data class Track(
        val id: Long, val uri: String, val title: String, val artist: String, val album: String, val albumArtist: String,
        val genre: String, val codec: String, val container: String, val year: Int, val trackNo: Int, val discNo: Int,
        val sampleRate: Int, val bitDepth: Int, val channels: Int, val durationSeconds: Double, val bitrateKbps: Int,
        val lossless: Boolean, val hasArtwork: Boolean,
    )

    data class Progress(val seen: Long, val added: Long, val updated: Long, val removed: Long, val failed: Long, val finished: Boolean)

    private val closed = AtomicBoolean(false)
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "GrooveLibrary").apply { isDaemon = true } }
    @Volatile private var progressRef: WeakReference<(Progress) -> Unit>? = null

    /** Index the files of one picked folder (SAF tree). Progress is delivered on the main thread. */
    fun scan(rootKey: String, entries: List<Entry>, onProgress: ((Progress) -> Unit)? = null): Boolean {
        if (closed.get()) return false
        progressRef = onProgress?.let { WeakReference(it) }
        return GrooveNative.nativeLibraryScan(
            handle, rootKey,
            Array(entries.size) { entries[it].uri }, Array(entries.size) { entries[it].folder },
            LongArray(entries.size) { entries[it].size }, LongArray(entries.size) { entries[it].mtime },
            WeakReference(this),
        )
    }

    fun cancelScan() { if (!closed.get()) GrooveNative.nativeLibraryCancel(handle) }

    /** Runs the smart playlist off the main thread and delivers the tracks on it. */
    fun query(playlist: SmartPlaylist, onResult: (List<Track>?) -> Unit) {
        worker.execute {
            val result = if (closed.get()) null else runQuery(playlist)
            main.post { onResult(result) }
        }
    }

    private fun runQuery(p: SmartPlaylist): List<Track>? {
        val n = p.rules.size
        val json = GrooveNative.nativeLibraryQuery(
            handle, p.matchAll,
            IntArray(n) { p.rules[it].field.ordinal }, IntArray(n) { p.rules[it].op.ordinal },
            Array(n) { p.rules[it].text }, DoubleArray(n) { p.rules[it].a }, DoubleArray(n) { p.rules[it].b },
            p.sortBy.ordinal, p.descending, p.limit, p.random,
        ) ?: return null
        val arr = JSONArray(json)
        return List(arr.length()) { i ->
            val o = arr.getJSONObject(i)
            Track(
                o.getLong("id"), o.getString("uri"), o.getString("title"), o.getString("artist"), o.getString("album"),
                o.getString("albumArtist"), o.getString("genre"), o.getString("codec"), o.getString("container"),
                o.getInt("year"), o.getInt("trackNo"), o.getInt("discNo"), o.getInt("sampleRate"), o.getInt("bitDepth"),
                o.getInt("channels"), o.getDouble("duration"), o.getInt("bitrate"), o.getBoolean("lossless"), o.getBoolean("hasArtwork"),
            )
        }
    }

    override fun close() {
        if (closed.compareAndSet(false, true)) {
            worker.shutdown()
            GrooveNative.nativeLibraryClose(handle)  // cancels and joins the scanner
            main.removeCallbacksAndMessages(null)
        }
    }

    // Called by native code on the scanner thread.
    @Suppress("unused") private fun onNativeScanProgress(seen: Long, added: Long, updated: Long, removed: Long, failed: Long, finished: Boolean) {
        val p = Progress(seen, added, updated, removed, failed, finished)
        main.post { if (!closed.get()) progressRef?.get()?.invoke(p) }
    }

    companion object {
        fun open(context: Context, dbFile: File = File(context.filesDir, "groove-library.db")): GrooveLibrary? {
            if (!GrooveNative.available) return null
            val h = GrooveNative.nativeLibraryOpen(dbFile.absolutePath, SafFileOpener(context.applicationContext))
            return if (h == 0L) null else GrooveLibrary(h)
        }

        /** Lists audio files under a SAF tree (call off the main thread). Same filter as the native scanner. */
        fun listTree(context: Context, treeUri: Uri, maxFiles: Int = 20_000): List<Entry> {
            val out = ArrayList<Entry>()
            val cr = context.contentResolver
            val audio = Regex("\\.(flac|wav|wave|aif|aiff|aifc|w64|caf|ogg|oga|opus|mp3|m4a|alac|aac|mp4|dsf|dff)$", RegexOption.IGNORE_CASE)
            fun walk(docId: String, folder: String, depth: Int) {
                if (depth > 16 || out.size >= maxFiles) return
                val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, docId)
                val cols = arrayOf(
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
                    DocumentsContract.Document.COLUMN_LAST_MODIFIED,
                )
                val dirs = ArrayList<Pair<String, String>>()
                cr.query(children, cols, null, null, null)?.use { c ->
                    while (c.moveToNext() && out.size < maxFiles) {
                        val id = c.getString(0) ?: continue
                        val name = c.getString(1) ?: continue
                        if (name.startsWith(".")) continue
                        if (c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) dirs += id to name
                        else if (audio.containsMatchIn(name)) out += Entry(
                            DocumentsContract.buildDocumentUriUsingTree(treeUri, id).toString(), folder,
                            if (c.isNull(3)) 0 else c.getLong(3), if (c.isNull(4)) 0 else c.getLong(4) / 1000,
                        )
                    }
                }
                for ((id, name) in dirs) walk(id, if (folder.isEmpty()) name else "$folder/$name", depth + 1)
            }
            walk(DocumentsContract.getTreeDocumentId(treeUri), "", 0)
            return out
        }
    }
}
