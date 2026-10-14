package org.capyps5.emulator.model

data class GameItem(
    val titleId: String,
    val titleName: String,
    val directoryPath: String,
    val elfPath: String,
    val iconUri: String? = null,
    val fileSizeFormatted: String = ""
)
