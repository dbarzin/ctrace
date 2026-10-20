# ctrace

*[English version](README.md)*

Instrumenteur source-à-source pour tracer pas à pas l'exécution d'un programme C.
Basé sur Clang LibTooling (AST + Rewriter).

```sh
ctrace a.c | gcc -x c - -o a.out     # ou : ctrace-cc a.c -o a.out
./a.out                              # trace sur stderr
CTRACE_OUT=trace.txt ./a.out         # trace dans un fichier
```

## Format de la trace

```
demo.c:38   s = add(s, 2);                // s=0     code exécuté // état AVANT
  demo.c:6  -> add(a=0, b=2)                         entrée de fonction + paramètres
  demo.c:8    return a + b;               // a=0 b=2
  demo.c:8  <- add = 2                               return + valeur
          => s=2                                     état APRÈS (variables modifiées)
demo.c:49   while (i < 3)                 // i=0
          -> true                                    résultat de la condition
```

Le commentaire `//` n'apparaît que si l'instruction lit des variables, et la
ligne `=>` que si elle en modifie.

L'indentation suit la profondeur d'appel.

## Construction

Dépendances : Clang/LLVM 18 ou plus récent avec les en-têtes (testé avec LLVM 19). Sous Debian/Ubuntu :

```sh
sudo apt install clang-19 libclang-19-dev llvm-19-dev
```

```sh
make                 # produit ./ctrace
make test            # instrumente et exécute examples/demo.c
make install         # /usr/local/bin/ctrace et ctrace-cc
```

Le Makefile utilise `llvm-config` s'il est dans le `PATH`, sinon le
`/usr/bin/llvm-config-NN` le plus récent, et le `clang` de la même installation.
Pour imposer une version : `make LLVM_CONFIG=llvm-config-19`.

Les options après le fichier sont passées à Clang : `ctrace a.c -Iinclude -DDEBUG`.

## Ce qui est tracé

| Élément | Avant | Après |
|---|---|---|
| expression, affectation, appel | variables lues | variables / lvalues modifiées (`x`, `t[i]`, `p->x`, `*p`) |
| déclaration avec initialiseur | variables lues | variables déclarées |
| `if`, `while`, `for`, `do`, `switch` | variables de la condition | `-> true/false` à chaque évaluation |
| incrément du `for` | variables | variables modifiées |
| entrée de fonction | paramètres | |
| `return` | variables de l'expression | valeur retournée |
| `break`, `continue`, `goto` | ligne | |

Affichage selon le type (décidé à l'instrumentation à partir de l'AST) :
entiers, `enum`, `bool`, `char`, flottants, pointeurs (adresse), tableaux de
taille fixe (8 premiers éléments), tableaux de `char` (chaîne bornée),
`struct` sur un niveau, `union` (premier membre). Les variables `struct` sont
affichées champ par champ (`pt.x=1 pt.y=2`), une `struct` retournée entre
accolades (`<- move = {x=11, y=2}`).

## Principe

1. Clang parse le fichier (préprocesseur inclus) et produit l'AST.
2. Pour chaque fonction du fichier principal, le Rewriter insère des appels au
   runtime `__ct_*` :
   - avant/après chaque instruction d'un bloc ;
   - dans les conditions sous forme d'expression à virgule :
     `if ((pre..., __ct_cv = !!(c), __ct_cond(__ct_cv)))` ;
   - dans les `return` : `return (pre..., __ct_r = (e), <- f = v, __ct_r);` ;
   - accolades ajoutées autour des corps sans `{ }`.
3. Aucun saut de ligne n'est inséré dans le code utilisateur et une directive
   `#line 1 "a.c"` suit le runtime : les erreurs de gcc et le débogage pointent
   vers les lignes d'origine.
4. Le runtime (`ctrace_runtime.inc`) est copié en tête de la sortie : aucune
   bibliothèque à lier. `__ct_depth` est `weak`, donc plusieurs `.c` instrumentés
   partagent la même profondeur d'appel.

Les sous-expressions non évaluées de façon certaine (`p && p->x = 1`, `?:`) ne
sont pas réévaluées dans l'état « après », pour ne pas déréférencer un pointeur nul.

## Limites connues (squelette)

- Pointeurs affichés par adresse uniquement (`char *` inclus) : lire la chaîne
  pointée pourrait planter sur un pointeur non initialisé.
- Une instruction entièrement produite par une macro (`assert(...)`) est tracée
  mais pas décomposée.
- Les initialisations et incréments à l'intérieur des conditions ne sont pas
  affichés en « après » pour `switch`.
- Pas de gestion des threads (la profondeur est globale).
- Les variables non initialisées apparaissent avec leur valeur indéterminée
  lorsqu'elles sont lues (c'est l'état réel).
- Les fonctions des en-têtes inclus ne sont pas instrumentées, seulement le
  fichier passé en argument.
