primacy(woman).
primacy(man).
exemplar(man, mark).
exemplar(man, ben).

alive(X, Y) :- primacy(X), exemplar(X, Y).



food(fruit, banana).
food(vegetable, onion).

eda(X, Y) :- food(X, Y).
