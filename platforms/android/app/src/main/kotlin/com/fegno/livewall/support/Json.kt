package com.fegno.livewall.support

/**
 * A small JSON reader and writer.
 *
 * The platform ships `org.json`, so this looks redundant — but `org.json` lives
 * in `android.jar`, which on a JVM unit test is a stub that throws
 * "not mocked" on every call. The library index is exactly the kind of thing
 * that should be covered by tests that need no device, and adding Robolectric or
 * a JSON dependency to a zero-dependency app to reach it is the wrong trade. The
 * Windows port hand-rolled its JSON for the same reason.
 *
 * Deliberately incomplete: no `\uXXXX` surrogate cleverness beyond the basic
 * escape, no exponent-free integer distinction. It reads what [Json.write]
 * produces plus ordinary hand-edited JSON, and rejects the rest.
 */
object Json {

    class ParseException(message: String) : Exception(message)

    // MARK: - Reading

    fun parse(text: String): Any? = Parser(text).run {
        val value = parseValue()
        skipWhitespace()
        if (!atEnd()) fail("trailing content")
        value
    }

    private class Parser(private val text: String) {
        private var index = 0

        fun atEnd() = index >= text.length

        fun fail(message: String): Nothing =
            throw ParseException("$message at offset $index")

        fun skipWhitespace() {
            while (index < text.length && text[index].isWhitespace()) index++
        }

        fun parseValue(): Any? {
            skipWhitespace()
            if (atEnd()) fail("unexpected end of input")
            return when (text[index]) {
                '{' -> parseObject()
                '[' -> parseArray()
                '"' -> parseString()
                't' -> literal("true", true)
                'f' -> literal("false", false)
                'n' -> literal("null", null)
                else -> parseNumber()
            }
        }

        private fun literal(word: String, value: Any?): Any? {
            if (!text.startsWith(word, index)) fail("bad literal")
            index += word.length
            return value
        }

        private fun parseObject(): Map<String, Any?> {
            index++ // '{'
            val result = LinkedHashMap<String, Any?>()
            skipWhitespace()
            if (!atEnd() && text[index] == '}') { index++; return result }
            while (true) {
                skipWhitespace()
                if (atEnd() || text[index] != '"') fail("expected a key")
                val key = parseString()
                skipWhitespace()
                if (atEnd() || text[index] != ':') fail("expected ':'")
                index++
                result[key] = parseValue()
                skipWhitespace()
                if (atEnd()) fail("unterminated object")
                when (text[index]) {
                    ',' -> index++
                    '}' -> { index++; return result }
                    else -> fail("expected ',' or '}'")
                }
            }
        }

        private fun parseArray(): List<Any?> {
            index++ // '['
            val result = ArrayList<Any?>()
            skipWhitespace()
            if (!atEnd() && text[index] == ']') { index++; return result }
            while (true) {
                result.add(parseValue())
                skipWhitespace()
                if (atEnd()) fail("unterminated array")
                when (text[index]) {
                    ',' -> index++
                    ']' -> { index++; return result }
                    else -> fail("expected ',' or ']'")
                }
            }
        }

        private fun parseString(): String {
            index++ // '"'
            val builder = StringBuilder()
            while (true) {
                if (atEnd()) fail("unterminated string")
                when (val c = text[index++]) {
                    '"' -> return builder.toString()
                    '\\' -> {
                        if (atEnd()) fail("unterminated escape")
                        when (val escape = text[index++]) {
                            '"' -> builder.append('"')
                            '\\' -> builder.append('\\')
                            '/' -> builder.append('/')
                            'b' -> builder.append('\b')
                            'f' -> builder.append('\u000C')
                            'n' -> builder.append('\n')
                            'r' -> builder.append('\r')
                            't' -> builder.append('\t')
                            'u' -> {
                                if (index + 4 > text.length) fail("truncated \\u escape")
                                val code = text.substring(index, index + 4).toIntOrNull(16)
                                    ?: fail("bad \\u escape")
                                builder.append(code.toChar())
                                index += 4
                            }
                            else -> fail("unknown escape '\\$escape'")
                        }
                    }
                    else -> builder.append(c)
                }
            }
        }

        private fun parseNumber(): Double {
            val start = index
            if (!atEnd() && (text[index] == '-' || text[index] == '+')) index++
            while (!atEnd() && (text[index].isDigit() || text[index] in ".eE+-")) index++
            if (start == index) fail("expected a value")
            return text.substring(start, index).toDoubleOrNull() ?: fail("bad number")
        }
    }

    // MARK: - Writing

    fun write(value: Any?, indent: Int = 0): String {
        val pad = "  ".repeat(indent)
        val inner = "  ".repeat(indent + 1)
        return when (value) {
            null -> "null"
            is String -> quote(value)
            is Boolean -> value.toString()
            is Int, is Long -> value.toString()
            is Double -> if (value == Math.floor(value) && !value.isInfinite())
                value.toLong().toString() else value.toString()
            is Map<*, *> -> if (value.isEmpty()) "{}" else value.entries.joinToString(
                separator = ",\n", prefix = "{\n", postfix = "\n$pad}"
            ) { "$inner${quote(it.key.toString())}: ${write(it.value, indent + 1)}" }
            is List<*> -> if (value.isEmpty()) "[]" else value.joinToString(
                separator = ",\n", prefix = "[\n", postfix = "\n$pad]"
            ) { "$inner${write(it, indent + 1)}" }
            else -> quote(value.toString())
        }
    }

    private fun quote(text: String): String {
        val builder = StringBuilder(text.length + 2)
        builder.append('"')
        for (c in text) {
            when {
                c == '"' -> builder.append("\\\"")
                c == '\\' -> builder.append("\\\\")
                c == '\n' -> builder.append("\\n")
                c == '\r' -> builder.append("\\r")
                c == '\t' -> builder.append("\\t")
                c < ' ' -> builder.append("\\u%04x".format(c.code))
                else -> builder.append(c)
            }
        }
        builder.append('"')
        return builder.toString()
    }
}

// MARK: - Typed accessors
//
// Every one of these returns null rather than throwing, because the index reader
// wants "this entry is unusable" as a value, not as control flow.

fun Any?.asMap(): Map<String, Any?>? {
    @Suppress("UNCHECKED_CAST")
    return this as? Map<String, Any?>
}

fun Any?.asList(): List<Any?>? = this as? List<*>

fun Map<String, Any?>.string(key: String): String? = this[key] as? String

fun Map<String, Any?>.int(key: String): Int? = (this[key] as? Double)?.toInt()

fun Map<String, Any?>.long(key: String): Long? = (this[key] as? Double)?.toLong()
