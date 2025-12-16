---
title: Memory Safety
permalink: /memory-safety/
nav_order: 17
---

# Memory Safety

## RAM watcher

u.m.p. tries to be a good citizen by monitoring the system RAM. If overall RAM exceeds `92%`, it pauses all RAM cache operations and displays a subtle warning in the app's menu area. It will resume again once the watcher sees a drop to `85%`. In the age of After Effects and other RAM-hungry apps, we are taking a brute force approach to playing it safe. 