package com.fegno.livewall

import com.fegno.livewall.support.Json
import com.fegno.livewall.support.asList
import com.fegno.livewall.support.asMap
import com.fegno.livewall.support.int
import com.fegno.livewall.support.long
import com.fegno.livewall.support.string
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class JsonTest {

    @Test
    fun `objects and arrays nest`() {
        val value = Json.parse("""{"a":[1,2,{"b":"c"}],"d":null,"e":true}""").asMap()!!
        assertEquals(3, value["a"].asList()!!.size)
        assertEquals("c", value["a"].asList()!![2].asMap()!!.string("b"))
        assertNull(value["d"])
        assertEquals(true, value["e"])
    }

    @Test
    fun `numbers come back as doubles and narrow on demand`() {
        val value = Json.parse("""{"n":42,"big":3500000000,"f":1.5}""").asMap()!!
        assertEquals(42, value.int("n"))
        assertEquals(3_500_000_000L, value.long("big"))
        assertEquals(1.5, value["f"])
    }

    @Test
    fun `escapes survive both directions`() {
        val awkward = "quote \" backslash \\ newline \n tab \t slash /"
        val text = Json.write(mapOf("s" to awkward))
        assertEquals(awkward, Json.parse(text).asMap()!!.string("s"))
    }

    @Test
    fun `unicode escapes are read`() {
        assertEquals("é", Json.parse("""["é"]""").asList()!![0])
    }

    @Test
    fun `whole doubles are written without a decimal point`() {
        // Otherwise every width in the index reads back as "1920.0", which
        // parses fine and looks like a bug in a file people do open.
        assertTrue(Json.write(mapOf("w" to 1920.0)).contains("\"w\": 1920\n"))
        assertTrue(Json.write(mapOf("f" to 1.5)).contains("\"f\": 1.5"))
    }

    @Test
    fun `empty containers have a compact form`() {
        assertEquals("[]", Json.write(emptyList<Any>()))
        assertEquals("{}", Json.write(emptyMap<String, Any>()))
    }

    @Test
    fun `malformed input throws rather than returning something plausible`() {
        for (bad in listOf("{", "[1,]", """{"a" 1}""", """{"a":}""", "tru", "", "[1] junk")) {
            assertThrows("accepted <$bad>", Json.ParseException::class.java) { Json.parse(bad) }
        }
    }

    @Test
    fun `typed accessors return null on the wrong type rather than throwing`() {
        val value = Json.parse("""{"a":"text","b":[1]}""").asMap()!!
        assertNull(value.int("a"))
        assertNull(value.string("b"))
        assertNull(value.long("missing"))
        assertNull("not a map".asMap())
        assertNull(42.0.asList())
    }

    @Test
    fun `written output is indented enough to hand-edit`() {
        val text = Json.write(listOf(mapOf("a" to 1.0)))
        assertTrue(text, text.contains("\n"))
        assertEquals(listOf(mapOf("a" to 1.0)), Json.parse(text))
    }
}
