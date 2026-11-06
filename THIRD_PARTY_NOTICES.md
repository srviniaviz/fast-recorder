# Third party notices

## whisper.cpp

Fast Record builds the [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
inference library from a pinned upstream revision. whisper.cpp is distributed
under the MIT License. Its license text is available in the upstream
repository's `LICENSE` file.

The Whisper Tiny, Base, and Small model weights are not bundled with Fast
Record. Users can choose which model to download from the
[ggml-org Whisper model repository](https://huggingface.co/ggerganov/whisper.cpp).
The app verifies the expected file size and SHA-256 digest before installing
each model in the user's local application data folder.
