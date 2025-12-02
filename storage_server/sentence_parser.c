#include "sentence_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Check if a sentence ends with a delimiter
int sentence_has_delimiter(const char *sentence) {
    if (!sentence || strlen(sentence) == 0) return 0;
    
    // Trim trailing whitespace
    int len = strlen(sentence);
    while (len > 0 && isspace(sentence[len - 1])) {
        len--;
    }
    
    if (len == 0) return 0;
    
    char last = sentence[len - 1];
    
    // Check if last character is a delimiter
    if (last != '.' && last != '!' && last != '?') {
        return 0;  // Not a delimiter
    }
    
    // Check if it's a SINGLE delimiter (not multiple like ... or !!!)
    // Look at the character before the last one
    if (len >= 2) {
        char second_last = sentence[len - 2];
        // If second-to-last is also a delimiter, this is NOT a sentence ending
        // (e.g., "..." or "!!!" should be treated as a word, not sentence ending)
        if (second_last == '.' || second_last == '!' || second_last == '?') {
            return 0;  // Multiple delimiters - treat as word, not sentence end
        }
    }
    
    // Single delimiter at the end - valid sentence ending
    return 1;
}

// Parse file into sentences (sentences end with SINGLE . ! ?)
// Multiple delimiters like ... or !!! are treated as words, not sentence endings
char** parse_sentences(const char *content, int *sentence_count) {
    if (!content || strlen(content) == 0) {
        *sentence_count = 0;
        return NULL;
    }
    
    // Check if content is only whitespace
    const char *check = content;
    int has_non_whitespace = 0;
    while (*check) {
        if (!isspace(*check)) {
            has_non_whitespace = 1;
            break;
        }
        check++;
    }
    
    if (!has_non_whitespace) {
        *sentence_count = 0;
        return NULL;
    }
    
    // Count sentences first
    // EVERY delimiter (., !, ?) creates a separate sentence
    // Example: "..." = 3 sentences: ".", ".", "."
    *sentence_count = 0;
    const char *p = content;
    int in_sentence = 0;
    
    while (*p) {
        if (*p == '.' || *p == '!' || *p == '?') {
            // EVERY delimiter creates a sentence boundary
            (*sentence_count)++;
            in_sentence = 0;
        } else if (!in_sentence && !isspace(*p)) {
            in_sentence = 1;
        }
        p++;
    }
    
    // If last sentence doesn't end with delimiter, count it
    if (in_sentence) {
        (*sentence_count)++;
    }
    
    if (*sentence_count == 0) {
        *sentence_count = 0;
        return NULL;
    }
    
    // Allocate array
    char **sentences = malloc(sizeof(char*) * (*sentence_count));
    
    // Extract sentences
    int idx = 0;
    const char *start = content;
    p = content;
    
    while (*p && idx < *sentence_count) {
        if (*p == '.' || *p == '!' || *p == '?') {
            // EVERY delimiter marks sentence boundary
            int len = p - start + 1;  // Include the delimiter
            sentences[idx] = malloc(len + 1);
            strncpy(sentences[idx], start, len);
            sentences[idx][len] = '\0';
            idx++;
            
            // Skip whitespace after delimiter
            p++;
            while (*p && isspace(*p)) p++;
            start = p;
        } else {
            p++;
        }
    }
    
    // Handle last sentence if it doesn't end with delimiter
    if (idx < *sentence_count && *start) {
        int len = strlen(start);
        sentences[idx] = malloc(len + 1);
        strcpy(sentences[idx], start);
    }
    
    return sentences;
}

// Parse sentence into words
char** parse_words(const char *sentence, int *word_count) {
    if (!sentence || strlen(sentence) == 0) {
        *word_count = 0;
        return NULL;
    }
    
    char *copy = strdup(sentence);
    *word_count = 0;
    
    // Count words
    char *token = strtok(copy, " \t\n");
    while (token != NULL) {
        (*word_count)++;
        token = strtok(NULL, " \t\n");
    }
    
    if (*word_count == 0) {
        free(copy);
        return NULL;
    }
    
    // Allocate array
    char **words = malloc(sizeof(char*) * (*word_count));
    
    // Extract words
    strcpy(copy, sentence);
    int idx = 0;
    token = strtok(copy, " \t\n");
    while (token != NULL && idx < *word_count) {
        words[idx] = strdup(token);
        idx++;
        token = strtok(NULL, " \t\n");
    }
    
    free(copy);
    return words;
}

// Rebuild sentence from words
char* rebuild_sentence(char **words, int word_count) {
    if (word_count == 0) return strdup("");
    
    int total_len = 0;
    for (int i = 0; i < word_count; i++) {
        total_len += strlen(words[i]) + 1;  // +1 for space
    }
    
    char *result = malloc(total_len + 1);
    result[0] = '\0';
    
    for (int i = 0; i < word_count; i++) {
        if (i > 0) strcat(result, " ");
        strcat(result, words[i]);
    }
    
    return result;
}
