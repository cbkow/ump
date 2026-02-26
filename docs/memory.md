---
title: Memory Safety
permalink: /memory/
nav_order: 15
---

# Memory Safety

## RAM Watcher

QCView monitors overall system RAM to avoid destabilizing systems running alongside other memory-intensive applications. If RAM usage exceeds `92%`, all cache operations are paused and a warning indicator appears in the menu bar. Once usage drops back to `85%`, caching resumes automatically.