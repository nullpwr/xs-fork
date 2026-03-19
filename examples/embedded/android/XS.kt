package org.xslang

class XS {
    init {
        System.loadLibrary("xs")
    }

    external fun eval(src: String): String
    external fun runBytecode(bytes: ByteArray): Int
}
