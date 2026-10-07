I vibecoded this raymarcher entirely on a local llm running on my RTX3090. The code is pure slop. It it is all implemented using a vulkan compute shader so it runs decently fast.

https://github.com/user-attachments/assets/f056aa8e-0b5b-4db9-bc92-0d6e2845b036

getting about 50 tokens/sec with speculative decoding, qwen 3.8 27B

It took a _lot_ of back-and-forth. kinda felt like using last year's sonnet, but the fact that it was running purely on my own machine is super cool. I plugged the local llm in as another backend for codex, so I can just use the chatgpt app on my phone to drive it.
