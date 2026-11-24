Hey everyone,

I wanted to share a plugin I’ve developed called **Unspin**, which is now available in the Unraid Apps tab.

# The Problem (Why I built this)

The primary reason this plugin exists is to **prevent unnecessary and noisy HDD spin-ups** while working on active files.

Normally, Unraid only moves files *down* from the cache to the array via the Mover. If you need to open and edit an old project stored on the array, you have two choices—and both are frustrating:

1. You deal with constant HDD latency, forcing your disks to spin up and stay active throughout your entire editing session.
2. You have to manually move the project files back to the cache yourself before starting.

Unspin automates this process cleanly to ensure that your array disks can spin down and stay asleep as much as possible.

# How Unspin Works

* **Passive Monitoring:** It monitors read activity passively using Linux `fanotify`.
* **Zero Unnecessary Spin-ups:** It doesn't spin up disks on its own. When you open a file and the disk spins up anyway, the plugin notes the activity.
* **Smart Promotion:** Once you close the file or pause editing (releasing the file handle), it automatically moves those frequently accessed files up to your SSD cache.
* **Silent Array:** Future reads then pull directly from the SSD. This allows the array disks to spin back down quickly and stay silent while you continue editing.
* **Cache protection:** Unspin watches the fill state of your cache and never promotes files above your set level.

# Pro-Tip for Setup

A great way to use it is paired with the **Mover Tuning** plugin. If you enable `atime` tracking there, the regular Mover won't immediately push your active projects back to the array during the nightly run.

Since it's a relatively new tool, I'd love to get some feedback from the community here. If you want to dive deeper, look over the features, or need troubleshooting help, please check out the official support thread on the [Unraid Forums](https://forums.unraid.net/topic/198091-plugin-unspin-rudimentary-hot-file-tiering-for-unraid/). The Github is [here](https://github.com/kaedinger/unspin).

Let me know what you think or if you have any questions!

**Edit: SSD wear**

Since this came up in the comments: Unspin copies a file to the cache once it qualifies as hot, and the file stays there while you work on it. On my box (software development, office work, and audio projects) that's at most 2–3 GB a day. To be generous, here's 10 GB/day too.

So here's how long a typical 1 TB SSD (rated for 600 TB written) would last before it reaches its wear rating (actual math in the comments):

|Cache writes without Unspin|life span without Unspin|with 3 GB/day added from Unspin|with 10 GB/day from Unspin|
|:-|:-|:-|:-|
|20 GB/day (quiet)|82 years|71 years (11 fewer)|55 years (27 fewer)|
|50 GB/day (busy)|33 years|31 years (2 fewer)|27 years (6 fewer)|

Every value there is far beyond how long anyone keeps a drive, but with Unspin the wear and tear of the HDDs is reduced.

Worth paying attention to: small QLC drives or old drives have much lower TBW ratings, so check yours. And avoid files bouncing back and forth (Unspin promotes a file, the mover moves it back, Unspin promotes it again). Mover Tuning's atime filter together with Unspin's "Pool Access Times" setting prevents exactly that. More details in the comments.

**AI Disclosure:** Claude helped restructure the code, clean up formatting, draft the initial documentation. However, the core logic was entirely human-written and vetted by me—originally built years ago for an old Ubuntu machine and a corporate project, now completely adapted for Unraid. It runs on my personal Unraid box and all tests were done by me. No raw AI slop here! AI also helped structuring this post.

Edit: AI disclosure, correct links, SSD wear, copy/move misnomer