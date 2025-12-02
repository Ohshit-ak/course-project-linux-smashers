#ifndef SENTENCE_PARSER_H
#define SENTENCE_PARSER_H

// Sentence and word parsing functions
int sentence_has_delimiter(const char *sentence);
char** parse_sentences(const char *content, int *sentence_count);
char** parse_words(const char *sentence, int *word_count);
char* rebuild_sentence(char **words, int word_count);

#endif // SENTENCE_PARSER_H
