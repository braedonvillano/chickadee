CS 161 Problem Set 3 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

Answers to written questions
----------------------------

# ALL

1. Because my design doc was made retroactively, it should reflect all final changes
in my VFS implementation. However, as a brief summary: I had to add read and write
functions for each of my vnode subclasses. I ended up putting a higher load on my
vnode_pipe because to simplify the read and write code. For example, I put integers
that specified the number of readers and writers, so that I could unblock a particular
end of the pipe when necessary.

Grading notes
-------------
