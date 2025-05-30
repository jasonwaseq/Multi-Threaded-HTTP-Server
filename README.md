# Multi-Threaded-HTTP-Server

## Purpose:
This assignment involves designing and implementing a robust HTTP server in C that uses a dispatcher thread alongside a configurable pool of worker threads to handle multiple client connections concurrently. Students extend a basic single-threaded server by integrating a thread-safe queue and reader–writer locks to ensure efficient, correct synchronization while processing GET and PUT requests. A key requirement is maintaining a durable, atomic audit log—written to stderr—that records each request’s operation, URI, status code, and optional RequestID header in a total order that linearizes concurrent interactions, thereby guaranteeing both performance and consistency.
