% типы музыки: электроника, рок
style(electronic).
style(rock).
% названия поджанров и их принадлежность к типу
subgenre(electronic,techno).
subgenre(electronic,trance).
subgenre(rock,metal).
subgenre(rock,punk).
% в какой стране зародился поджанр
origin(techno,germany).
origin(trance,germany).
origin(metal,england).
origin(punk,england).
% на каких инструментах играют
instrument(techno,synthesizer).
instrument(techno,drum_machine).
instrument(trance,synthesizer).
instrument(trance,sequencer).
instrument(metal,electric_guitar).
instrument(metal,drums).
instrument(punk,electric_guitar).
instrument(punk,bass_guitar).
instrument(punk,drums).
% предикат, показывающий инфу о поджанре
% X - тип (электроника/рок)
% Y - название поджанра
% Z - страна происхождения
% E - инструмент
genre_info(X,Y,Z,E):-
style(X),
subgenre(X,Y),
origin(Y,Z),
instrument(Y,E).
% эпоха: классика/современность, название поджанра
era(classic,punk).
era(classic,metal).
era(modern,techno).
era(modern,trance).
% подробная инфа о поджанре: название, настроение, темп, ритмический рисунок
traits(punk,raw,fast,threechord).
traits(metal,aggressive,fast,riffbased).
traits(techno,hypnotic,fast,fourfour).
traits(trance,uplifting,medium,fourfour).
% предикат, показывающий инфу о поджанре
% X - эпоха (классика/современность)
% Y - название поджанра
% A - настроение
% B - темп
% C - ритмический рисунок
genre_traits(X,Y,A,B,C):-
era(X,Y),
traits(Y,A,B,C).
